/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */
#include "vhmc_state_manager.hpp"

#include "xyz/openbmc_project/Common/error.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/exception.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace phosphor
{
namespace state
{
namespace manager
{

std::string instantiate(std::string_view name, std::string_view instance)
{
    return std::vformat(name, std::make_format_args(instance));
}

namespace server = sdbusplus::server::xyz::openbmc_project::state;

// Defined here rather than pulled from utils: nothing else from it is needed.
constexpr auto SYSTEMD_SERVICE = "org.freedesktop.systemd1";
constexpr auto SYSTEMD_OBJ_PATH = "/org/freedesktop/systemd1";
constexpr auto SYSTEMD_MANAGER_INTERFACE = "org.freedesktop.systemd1.Manager";
constexpr auto SYSTEMD_UNIT_INTERFACE = "org.freedesktop.systemd1.Unit";
constexpr auto PROPERTY_INTERFACE = "org.freedesktop.DBus.Properties";

// Job mode: replace a queued conflicting job rather than race it.
constexpr auto JOB_MODE = "replace";

bool VHMC::callSystemdUnitMethod(const char* method, const std::string& unit)
{
    auto call = this->bus.new_method_call(SYSTEMD_SERVICE, SYSTEMD_OBJ_PATH,
                                          SYSTEMD_MANAGER_INTERFACE, method);
    call.append(unit, JOB_MODE);

    try
    {
        this->bus.call(call);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error("{METHOD} on {UNIT} failed: {ERROR}", "METHOD",
                   std::string{method}, "UNIT", unit, "ERROR", e);
        return false;
    }

    return true;
}

bool VHMC::startUnit(const std::string& unit)
{
    return callSystemdUnitMethod("StartUnit", unit);
}

std::string VHMC::unitActiveState() const
{
    std::variant<std::string> state;

    auto call = this->bus.new_method_call(SYSTEMD_SERVICE, SYSTEMD_OBJ_PATH,
                                          SYSTEMD_MANAGER_INTERFACE, "GetUnit");
    call.append(unitName);

    sdbusplus::message::object_path unitPath;
    try
    {
        auto reply = this->bus.call(call);
        reply.read(unitPath);
    }
    catch (const sdbusplus::exception_t& e)
    {
        // Not loaded is the normal state before the first start.
        lg2::debug("Unit {UNIT} not found: {ERROR}", "UNIT", unitName, "ERROR",
                   e);
        return {};
    }

    auto get = this->bus.new_method_call(
        SYSTEMD_SERVICE, static_cast<const std::string&>(unitPath).c_str(),
        PROPERTY_INTERFACE, "Get");
    get.append(SYSTEMD_UNIT_INTERFACE, "ActiveState");

    try
    {
        auto reply = this->bus.call(get);
        reply.read(state);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error("Reading ActiveState of {UNIT} failed: {ERROR}", "UNIT",
                   unitName, "ERROR", e);
        return {};
    }

    return std::get<std::string>(state);
}

VHMC::BMCState VHMC::currentBMCState() const
{
    const auto active = unitActiveState();

    if (active == "active" || active == "reloading")
    {
        return BMCState::Ready;
    }
    if (active == "failed")
    {
        return BMCState::Quiesced;
    }

    // inactive, activating, deactivating, or unknown.
    return BMCState::NotReady;
}

bool VHMC::restartContainer()
{
    const std::string& unit = unitName;
    lg2::info("Restarting vHMC container unit {UNIT}", "UNIT", unit);

    if (!callSystemdUnitMethod("RestartUnit", unit))
    {
        return false;
    }

    // Re-arm the trigger, or the next image change would not restart it.
    return startUnit(triggerUnit);
}

bool VHMC::stopContainer()
{
    const std::string& unit = unitName;
    lg2::info("Stopping vHMC container unit {UNIT}", "UNIT", unit);

    // The trigger re-activates on inactive, so a shutdown only sticks after it.
    if (!callSystemdUnitMethod("StopUnit", triggerUnit))
    {
        lg2::error("Stopping {UNIT} failed; refusing to report the container "
                   "as stopped",
                   "UNIT", triggerUnit);
        return false;
    }

    return callSystemdUnitMethod("StopUnit", unit);
}

bool VHMC::applyReset(const char* tier)
{
    const std::filesystem::path flag{resetFlag};
    lg2::info("vHMC {TIER} reset requested", "TIER", std::string{tier});

    // Written here first, then renamed, so nobody reads a half-written tier.
    std::filesystem::path tmp{flag};
    tmp += ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(flag.parent_path(), ec);
    {
        std::ofstream out{tmp, std::ios::trunc};
        out << tier << '\n';
        if (!out)
        {
            lg2::error("Arming {FLAG} failed", "FLAG", flag.string());
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, flag, ec);
    if (ec)
    {
        lg2::error("Arming {FLAG} failed: {ERROR}", "FLAG", flag.string(),
                   "ERROR", ec.message());
        std::filesystem::remove(tmp, ec);
        return false;
    }

    // Restart, not stop-then-start: the hook runs even from a stopped state.
    if (!restartContainer())
    {
        std::filesystem::remove(flag, ec);
        return false;
    }

    // Armed and enqueued; awaiting the hook would outlast the timeout.
    return true;
}

VHMC::Transition VHMC::requestedBMCTransition(Transition value)
{
    bool ok = false;

    switch (value)
    {
        case Transition::None:
            return server::BMC::requestedBMCTransition(value);

        // Equivalent for a container: no in-guest shutdown to negotiate.
        case Transition::Reboot:
        case Transition::HardReboot:
            ok = restartContainer();
            break;

        case Transition::PowerOff:
            ok = stopContainer();
            break;

        default:
            lg2::error("Unsupported transition requested");
            break;
    }

    if (!ok)
    {
        // Leave it at None so a failure is not read as still in flight.
        server::BMC::requestedBMCTransition(Transition::None);
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }

    server::BMC::requestedBMCTransition(value);
    return server::BMC::requestedBMCTransition(Transition::None);
}

void VHMC::reset()
{
    if (!applyReset("factory"))
    {
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }
}

void VHMC::completeReset()
{
    if (!applyReset("complete"))
    {
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }
}

} // namespace manager
} // namespace state
} // namespace phosphor
