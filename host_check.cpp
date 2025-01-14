#include "config.h"

#include "host_check.hpp"

#include <unistd.h>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <xyz/openbmc_project/Condition/HostFirmware/client.hpp>
#include <xyz/openbmc_project/ObjectMapper/client.hpp>
#include <xyz/openbmc_project/State/Chassis/client.hpp>

#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <ranges>
#include <thread>
#include <vector>

namespace phosphor
{
namespace state
{
namespace manager
{

PHOSPHOR_LOG2_USING;

using namespace std::literals;

using ObjectMapper = sdbusplus::client::xyz::openbmc_project::ObjectMapper<>;
using Chassis = sdbusplus::client::xyz::openbmc_project::state::Chassis<>;
using HostFirmware =
    sdbusplus::client::xyz::openbmc_project::condition::HostFirmware<>;

// Required strings for sending the msg to check on host
constexpr auto CONDITION_HOST_PROPERTY = "CurrentFirmwareCondition";
constexpr auto PROPERTY_INTERFACE = "org.freedesktop.DBus.Properties";

constexpr auto CHASSIS_STATE_SVC = "xyz.openbmc_project.State.Chassis";
constexpr auto CHASSIS_STATE_POWER_PROP = "CurrentPowerState";

// Find all implementations of Condition interface and check if host is
// running over it
bool checkFirmwareConditionRunning(sdbusplus::bus_t& bus)
{
    // Find all implementations of host firmware condition interface
    auto mapper = bus.new_method_call(ObjectMapper::default_service,
                                      ObjectMapper::instance_path,
                                      ObjectMapper::interface, "GetSubTree");

    mapper.append("/", 0, std::vector<std::string>({HostFirmware::interface}));

    std::map<std::string, std::map<std::string, std::vector<std::string>>>
        mapperResponse;

    try
    {
        auto mapperResponseMsg = bus.call(mapper);
        mapperResponseMsg.read(mapperResponse);
    }
    catch (const sdbusplus::exception_t& e)
    {
        error(
            "Error in mapper GetSubTree call for HostFirmware condition: {ERROR}",
            "ERROR", e);
        throw;
    }

    if (mapperResponse.empty())
    {
        info("Mapper response for HostFirmware conditions is empty!");
        return false;
    }

    // Now read the CurrentFirmwareCondition from all interfaces we found
    // Currently there are two implementations of this interface. One by IPMI
    // and one by PLDM. The IPMI interface does a realtime check with the host
    // when the interface is called. This means if the host is not running,
    // we will have to wait for the timeout (currently set to 3 seconds). The
    // PLDM interface reads a cached state. The PLDM service does not put itself
    // on D-Bus until it has checked with the host. Therefore it's most
    // efficient to call the PLDM interface first. Do that by going in reverse
    // of the interfaces returned to us (PLDM will be last if available)
    for (const auto& [path, services] : std::views::reverse(mapperResponse))
    {
        for (const auto& serviceIter : services)
        {
            const std::string& service = serviceIter.first;

            try
            {
                auto method = bus.new_method_call(service.c_str(), path.c_str(),
                                                  PROPERTY_INTERFACE, "Get");
                method.append(HostFirmware::interface, CONDITION_HOST_PROPERTY);

                auto response = bus.call(method);
                std::variant<HostFirmware::FirmwareCondition> currentFwCondV;
                response.read(currentFwCondV);
                auto currentFwCond =
                    std::get<HostFirmware::FirmwareCondition>(currentFwCondV);

                info(
                    "Read host fw condition {COND_VALUE} from {COND_SERVICE}, {COND_PATH}",
                    "COND_VALUE", currentFwCond, "COND_SERVICE", service,
                    "COND_PATH", path);

                if (currentFwCond == HostFirmware::FirmwareCondition::Running)
                {
                    return true;
                }
            }
            catch (const sdbusplus::exception_t& e)
            {
                error("Error reading HostFirmware condition, error: {ERROR}, "
                      "service: {SERVICE} path: {PATH}",
                      "ERROR", e, "SERVICE", service, "PATH", path);
                throw;
            }
        }
    }
    return false;
}

// Helper function to check if chassis power is on
bool isChassiPowerOn(sdbusplus::bus_t& bus, size_t id)
{
    auto svcname = std::string{CHASSIS_STATE_SVC} + std::to_string(id);
    auto objpath = std::string{Chassis::namespace_path::value} + "/" +
                   std::string{Chassis::namespace_path::chassis} +
                   std::to_string(id);

    try
    {
        auto method = bus.new_method_call(svcname.c_str(), objpath.c_str(),
                                          PROPERTY_INTERFACE, "Get");
        method.append(Chassis::interface, CHASSIS_STATE_POWER_PROP);

        auto response = bus.call(method);
        std::variant<Chassis::PowerState> currentPowerStateV;
        response.read(currentPowerStateV);

        auto currentPowerState =
            std::get<Chassis::PowerState>(currentPowerStateV);

        if (currentPowerState == Chassis::PowerState::On)
        {
            return true;
        }
    }
    catch (const sdbusplus::exception_t& e)
    {
        error("Error reading Chassis Power State, error: {ERROR}, "
              "service: {SERVICE} path: {PATH}",
              "ERROR", e, "SERVICE", svcname.c_str(), "PATH", objpath.c_str());
        throw;
    }
    return false;
}

bool isHostRunning(size_t id)
{
    info("Check if host is running");

    auto bus = sdbusplus::bus::new_default();

    // No need to check if chassis power is not on
    if (!isChassiPowerOn(bus, id))
    {
        info("Chassis power not on, exit");
        return false;
    }

    // This applications systemd service is setup to only run after all other
    // application that could possibly implement the needed interface have
    // been started. However, the use of mapper to find those interfaces means
    // we have a condition where the interface may be on D-Bus but not stored
    // within mapper yet. There are five built in retries to check if it's
    // found the host is not up. This service is only called if chassis power
    // is on when the BMC comes up, so this won't impact most normal cases
    // where the BMC is rebooted with chassis power off. In cases where
    // chassis power is on, the host is likely running so we want to be sure
    // we check all interfaces
    for (int i = 0; i < 5; i++)
    {
        debug(
            "Introspecting new bus objects for bus id: {ID} sleeping for 1 second.",
            "ID", id);
        // Give mapper a small window to introspect new objects on bus
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        try
        {
            if (checkFirmwareConditionRunning(bus))
            {
                info("Host is running!");
                // Create file for host instance and create in filesystem to
                // indicate to services that host is running
                std::string hostFile = std::format(HOST_RUNNING_FILE, 0);
                std::ofstream outfile(hostFile);
                outfile.close();
                return true;
            }
        }
        catch (const sdbusplus::exception_t& e)
        {
            // sdbusplus exception throwed when dbus isn't ready,
            // sleep and retry
            continue;
        }
    }
    info("Host is not running!");
    return false;
}

} // namespace manager
} // namespace state
} // namespace phosphor
