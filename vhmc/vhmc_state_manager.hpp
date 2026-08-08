/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "config.h"

#include "com/nvidia/Common/CompleteReset/server.hpp"
#include "xyz/openbmc_project/Common/FactoryReset/server.hpp"
#include "xyz/openbmc_project/State/BMC/server.hpp"

#include <sdbusplus/bus.hpp>

#include <string>
#include <string_view>

namespace phosphor
{
namespace state
{
namespace manager
{

/** @brief Substitutes the instance for "{}"; other names pass through. */
std::string instantiate(std::string_view name, std::string_view instance);

using VHMCInherit = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::state::BMC,
    sdbusplus::server::xyz::openbmc_project::common::FactoryReset,
    sdbusplus::server::com::nvidia::common::CompleteReset>;

/** @brief Turns lifecycle and reset requests into systemd operations. */
class VHMC : public VHMCInherit
{
  public:
    /** @brief Constructs the manager for one container instance. */
    VHMC(sdbusplus::bus_t& bus, const char* objPath,
         std::string_view instance) :
        VHMCInherit(bus, objPath, VHMCInherit::action::defer_emit), bus(bus),
        unitName(instantiate(VHMC_UNIT_NAME, instance)),
        triggerUnit(instantiate(VHMC_TRIGGER_UNIT, instance)),
        resetFlag(instantiate(VHMC_RESET_FLAG, instance))
    {
        emit_object_added();
    }

    /** @brief Reboot and HardReboot restart the unit, PowerOff stops it. */
    Transition requestedBMCTransition(Transition value) override;

    /** @brief Reports container state, derived from the unit ActiveState. */
    BMCState currentBMCState() const override;

    /** @brief Clears the vHMC writable state, preserving its event log. */
    void reset() override;

    /** @brief Clears the vHMC writable state and its event log. */
    void completeReset() override;

    /** @brief Restarts the container and re-arms its start trigger. */
    bool restartContainer();

    /** @brief Stops the start trigger, then the container. */
    bool stopContainer();

    /** @brief Arms a reset tier and restarts the container to apply it. */
    bool applyReset(const char* tier);

  private:
    /** @brief Invokes a systemd method on a unit; false on failure. */
    bool callSystemdUnitMethod(const char* method, const std::string& unit);

    /** @brief Starts a one-shot unit, returning false on failure. */
    bool startUnit(const std::string& unit);

    /** @brief Reads ActiveState of the container unit; empty when unknown. */
    std::string unitActiveState() const;

    /** @brief Persistent sdbus connection. */
    sdbusplus::bus_t& bus;

    /** @brief Container unit this instance drives. */
    const std::string unitName;

    /** @brief Path unit that starts the container on an image change. */
    const std::string triggerUnit;

    /** @brief File the start hook reads the armed reset tier from. */
    const std::string resetFlag;
};

} // namespace manager
} // namespace state
} // namespace phosphor
