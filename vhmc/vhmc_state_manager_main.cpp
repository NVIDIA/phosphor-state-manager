/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */
#include "vhmc_state_manager.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <cstdlib>
#include <exception>
#include <string>

int main(int argc, char** argv)
try
{
    // systemd passes %i for a templated unit; a plain unit gets the default.
    const std::string instance{argc > 1 ? argv[1] : VHMC_DEFAULT_INSTANCE};

    const std::string objPath =
        phosphor::state::manager::instantiate(VHMC_OBJPATH, instance);
    const std::string busName =
        phosphor::state::manager::instantiate(VHMC_BUSNAME, instance);

    auto bus = sdbusplus::bus::new_default();

    // A lookup key only, named for the container, not the Redfish ManagerId.
    sdbusplus::server::manager_t objManager(bus, objPath.c_str());

    phosphor::state::manager::VHMC manager(bus, objPath.c_str(), instance);

    bus.request_name(busName.c_str());

    while (true)
    {
        bus.process_discard();
        bus.wait();
    }
}
catch (const std::exception& e)
{
    lg2::error("vhmc-state-manager terminated by exception: {ERR}", "ERR",
               e.what());
    return EXIT_FAILURE;
}
catch (...)
{
    lg2::error("vhmc-state-manager terminated by unknown exception");
    return EXIT_FAILURE;
}
