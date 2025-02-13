#pragma once

#include <sdbusplus/bus.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

namespace phosphor
{
namespace state
{
namespace manager
{
namespace utils
{

using PropertyValue = std::variant<int, std::string, bool>;
/** @brief Tell systemd to generate d-bus events
 *
 * @param[in] bus          - The Dbus bus object
 *
 * @return void, will throw exception on failure
 */
void subscribeToSystemdSignals(sdbusplus::bus_t& bus);

/** @brief Get service name from object path and interface
 *
 * @param[in] bus          - The Dbus bus object
 * @param[in] path         - The Dbus object path
 * @param[in] interface    - The Dbus interface
 *
 * @return The name of the service
 */
std::string getService(sdbusplus::bus_t& bus, std::string path,
                       std::string interface);

/** @brief Get the value of input property
 *
 * @param[in] bus          - The Dbus bus object
 * @param[in] path         - The Dbus object path
 * @param[in] interface    - The Dbus interface
 * @param[in] property     - The property name to get
 *
 * @return The value of the property
 */
std::string getProperty(sdbusplus::bus_t& bus, const std::string& path,
                        const std::string& interface,
                        const std::string& propertyName);

/** @brief Set the value of property
 *
 * @param[in] bus          - The Dbus bus object
 * @param[in] path         - The Dbus object path
 * @param[in] interface    - The Dbus interface
 * @param[in] property     - The property name to set
 * @param[in] value        - The value of property
 */
void setProperty(sdbusplus::bus_t& bus, const std::string& path,
                 const std::string& interface, const std::string& property,
                 const std::string& value);

/** @brief Return the value of the input GPIO
 *
 * @param[in] gpioName          - The name of the GPIO to read
 *
 *  * @return The value of the gpio (0 or 1) or -1 on error
 */
int getGpioValue(const std::string& gpioName);

/** @brief Create an error log
 *
 * @param[in] bus           - The Dbus bus object
 * @param[in] errorMsg      - The error message
 * @param[in] errLevel      - The error level
 * parampin] additionalData - Optional extra data to add to the log
 */
void createError(
    sdbusplus::bus_t& bus, const std::string& errorMsg,
    sdbusplus::server::xyz::openbmc_project::logging::Entry::Level errLevel,
    std::map<std::string, std::string> additionalData = {});

/** @brief Call phosphor-dump-manager to create BMC user dump
 *
 * @param[in] bus          - The Dbus bus object
 */
void createBmcDump(sdbusplus::bus_t& bus);

/** @brief Attempt to locate the obmc-chassis-lost-power@ file
 *    to indicate that an AC loss occurred.
 *
 * @param[in] chassisId  - the chassis instance
 */
bool checkACLoss(size_t& chassisId);

/** @brief Get the value of property
 *
 * @param[in] bus          - The Dbus bus object
 * @param[in] path         - The Dbus object path
 * @param[in] interface    - The Dbus interface
 * @param[in] property     - The property name to set
 * @param[in] value        - The value of property
 */
PropertyValue getPropertyV2(sdbusplus::bus::bus& bus, const std::string& path,
                            const std::string& interface,
                            const std::string& property);
/** @brief Determine if the BMC is at its Ready state
 *
 * @param[in] bus          - The Dbus bus object
 */
bool isBmcReady(sdbusplus::bus_t& bus);

/** @brief Wait for power restore delay and check for BMC_READY.
 *
 * @param[in] bus          - The Dbus bus object
 * @param[in] timeout      - Timeout in second
 */
bool waitForPowerDelayRestore(sdbusplus::bus_t& bus,
                              std::chrono::seconds timeout);

} // namespace utils
} // namespace manager
} // namespace state
} // namespace phosphor
