#include "config.h"

#include "bmc_state_manager.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <cstdlib>
#include <exception>

using BMCState = sdbusplus::server::xyz::openbmc_project::state::BMC;

int main()
try
{
    auto bus = sdbusplus::bus::new_default();

    // For now, we only have one instance of the BMC
    // 0 is for the current instance
    const auto* objPath = BMCState::namespace_path::value;
    sdbusplus::object_path objPathInst =
        sdbusplus::object_path(objPath) / BMCState::namespace_path::bmc;

    // Add sdbusplus ObjectManager.
    sdbusplus::server::manager_t objManager(bus, objPath);

    phosphor::state::manager::BMC manager(bus, objPathInst);

    bus.request_name(BMCState::interface);

    while (true)
    {
        bus.process_discard();
        bus.wait();
    }
}
catch (const std::exception& e)
{
    lg2::error("bmc-state-manager terminated by exception: {ERR}", "ERR",
               e.what());
    return EXIT_FAILURE;
}
catch (...)
{
    lg2::error("bmc-state-manager terminated by unknown exception");
    return EXIT_FAILURE;
}
