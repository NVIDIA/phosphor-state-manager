/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"

#include "configurable_state_manager.hpp"
#include "utils.hpp"

#include <boost/format.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/asio/connection.hpp> // Include the asio/connection header
#include <sdbusplus/asio/object_server.hpp> // Include the asio/object_server header
#include <sdbusplus/asio/property.hpp>      // Include the asio/property header
#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/server.hpp>
#include <sdbusplus/server/manager.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

namespace fs = std::filesystem;
using namespace phosphor::logging;

namespace configurable_state_manager
{

using namespace phosphor::logging;

constexpr auto SYSTEMD_SERVICE = "org.freedesktop.systemd1";
constexpr auto SYSTEMD_OBJ_PATH = "/org/freedesktop/systemd1";
constexpr auto SYSTEMD_INTERFACE = "org.freedesktop.systemd1.Manager";

phosphor::state::manager::utils::PropertyValue
    StateMachineHandler::handleTimeoutRetries(sdbusplus::bus::bus& bus,
                                              const std::string& service,
                                              const std::string& objectPath,
                                              const std::string& interface,
                                              const std::string& property)
{
    constexpr int maxRetries = 4; // Maximum number of retries
    int retryCount = 0;           // Track retry attempts

    while (retryCount < maxRetries)
    {
        try
        {
            // Attempt to fetch the property
            auto propertyValue = phosphor::state::manager::utils::getPropertyV2(
                bus, service, objectPath, interface, property);

            // Log success and return the retrieved value
            log<level::INFO>(
                (boost::format(
                     "Successfully retrieved property '%s' from object '%s', interface '%s' on retry attempt %d.") %
                 property % objectPath % interface % (retryCount + 1))
                    .str()
                    .c_str());
            return propertyValue;
        }
        catch (const sdbusplus::exception::SdBusError& e)
        {
            if (std::string(e.name()) == "org.freedesktop.DBus.Error.Timeout")
            {
                log<level::WARNING>(
                    (boost::format(
                         "Timeout occurred while fetching property '%s' from object '%s', interface '%s'. Retry %d/%d.") %
                     property % objectPath % interface % (retryCount + 1) %
                     maxRetries)
                        .str()
                        .c_str());
            }
            else
            {
                log<level::ERR>(
                    (boost::format(
                         "Unexpected error while fetching property '%s' from object '%s', interface '%s': %s.") %
                     property % objectPath % interface % e.what())
                        .str()
                        .c_str());
                throw; // Rethrow the exception for non-timeout errors
            }
        }

        // Increment retry count and wait before the next attempt
        ++retryCount;
    }

    // If all retries fail, throw an exception
    log<level::ERR>(
        (boost::format(
             "Failed to retrieve property '%s' from object '%s', interface '%s' after %d attempts. Throwing exception.") %
         property % objectPath % interface % maxRetries)
            .str()
            .c_str());

    throw std::runtime_error(
        (boost::format(
             "Unable to retrieve property '%s' from object '%s', interface '%s' after %d attempts.") %
         property % objectPath % interface % maxRetries)
            .str());
}

bool StateMachineHandler::any(const std::vector<bool>& bool_vector)
{
    for (bool value : bool_vector)
    {
        if (value)
        {
            return true;
        }
    }
    return false;
}

bool StateMachineHandler::all(const std::vector<bool>& bool_vector)
{
    for (bool value : bool_vector)
    {
        if (!value)
        {
            return false;
        }
    }
    return true;
}

void StateMachineHandler::init(sdbusplus::bus::bus& bus)
{
    // Collect object-interface pairs for all states
    std::unordered_map<std::string, std::unordered_set<std::string>>
        intfObjPairs;
    for (const auto& state : states)
    {
        collectMatchPairs(state.conditions, intfObjPairs);
    }
    // Register signal handlers before executing initial transition
    for (const auto& [ifaceName, objects] : intfObjPairs)
    {
        for (const auto& objPath : objects)
        {
            auto matchPtr = std::make_unique<sdbusplus::bus::match::match>(
                sdbusplus::bus::match::match(
                    bus,
                    sdbusplus::bus::match::rules::propertiesChanged(
                        std::string(objPath), ifaceName),
                    [&](sdbusplus::message::message& msg) {
                try
                {
                    // Execute the transition when properties change
                    executeTransition();
                    // for logging
                    log<level::INFO>(
                        std::format(
                            "Property change triggered state transition, Sender: '{}'",
                            msg.get_sender())
                            .c_str());
                }
                catch (const sdbusplus::exception::SdBusError& e)
                {
                    log<level::ERR>("Unable to execute Transiton",
                                    entry("ERR=%s msg=", e.what()));
                }
            }));

            eventHandlerMatcher.push_back(std::move(matchPtr));

            // create interface added matchPtr
            auto matchPtr2 = std::make_unique<sdbusplus::bus::match::match>(
                sdbusplus::bus::match::match(
                    bus,
                    sdbusplus::bus::match::rules::interfacesAdded() +
                        sdbusplus::bus::match::rules::argNpath(
                            0, std::string(objPath)),
                    [this, ifaceName](sdbusplus::message::message& msg) {
                std::map<std::string,
                         std::map<std::string, std::variant<std::string>>>
                    interfacesMap;
                sdbusplus::message::object_path path;
                msg.read(path, interfacesMap);

                for (auto& interface : interfacesMap)
                {
                    if (interface.first != ifaceName)
                    {
                        continue;
                    }

                    try
                    {
                        // Execute the transition when properties change
                        executeTransition();
                        log<level::INFO>(
                            std::format(
                                "Property change triggered state transition, Sender: '{}'",
                                msg.get_sender())
                                .c_str());
                    }
                    catch (const sdbusplus::exception::SdBusError& e)
                    {
                        log<level::ERR>(
                            "Unable to execute Transiton for interface added matchPtr",
                            entry("ERR=%s msg=", e.what()));
                    }
                }
            }));

            // insert interface added matchPtr
            eventHandlerMatcher.push_back(std::move(matchPtr2));
        }
    }
}

void StateMachineHandler::collectMatchPairs(
    const Condition& condition,
    std::unordered_map<std::string, std::unordered_set<std::string>>&
        intfObjPairs)
{
    // Add the current condition's object-interface pair if object is not empty
    if (!condition.object.empty() && !condition.intf.empty())
    {
        intfObjPairs[condition.intf].insert(condition.object);
    }

    // Process each sub-condition
    for (const auto& cond : condition.subConditions)
    {
        collectMatchPairs(cond, intfObjPairs);
    }
}

bool StateMachineHandler::evaluateCondition(sdbusplus::bus::bus& bus,
                                            const Condition& condition)
{
    // For simple condition type, check property value against target value
    if (condition.subConditions.empty())
    {
        // variable to hold output for getProperty
        phosphor::state::manager::utils::PropertyValue tmp;

        // find the service name containing object, intf
        std::string service;

        try
        {
            service = phosphor::state::manager::utils::getService(
                bus, condition.object, condition.intf);
            if (service.empty())
            {
                throw std::runtime_error(
                    "Empty service returned for object and interface");
            }
        }
        catch (const std::exception& e)
        {
            if (!condition.service.empty())
            {
                service = condition.service;
                log<level::INFO>("Falling back to service specified in config");
            }
            else
            {
                throw std::runtime_error(
                    "Failed to get service and no fallback service provided");
            }
        }
        log<level::INFO>(
            std::format("service name fetched: '{}'", service).c_str());

        try
        {
            // if the service hosting the object is csm look in local cache
            if (service.find("ConfigurableStateManager") != std::string::npos)
            {
                // if property hosted on same service use local cache
                // this is kind of local get operation
                tmp = localCache[condition.object];
            }
            else
            {
                tmp = handleTimeoutRetries(bus, service, condition.object,
                                           condition.intf, condition.property);
            }
        }
        catch (const std::exception& e)
        {
            auto errStrPath = std::format(
                "Got error with getProperty() with multiple tries for combination objectPath::{}, interface::{}, property::{}, with exception:: [E]:{}, hence setting state as default state",
                condition.object, condition.intf, condition.property, e.what());
            log<level::ERR>(errStrPath.c_str());
            throw std::runtime_error("Failed to get property");
        }

        std::string reqValue;

        if (std::holds_alternative<int>(tmp))
        {
            int intValue = std::get<int>(tmp);
            reqValue = std::to_string(intValue);
        }
        else if (std::holds_alternative<std::string>(tmp))
        {
            reqValue = std::get<std::string>(tmp);
        }
        else if (std::holds_alternative<bool>(tmp))
        {
            bool boolValue = std::get<bool>(tmp);
            reqValue = boolValue ? "true" : "false";
        }
        else
        {
            reqValue = "Unsupported Type";
        }

        log<level::INFO>(
            std::format(
                "Parsed property: '{}' from object: '{}', interface: '{}' with value: {}",
                condition.property, condition.object, condition.intf, reqValue)
                .c_str());

        return (condition.value.compare(reqValue) == 0);
    }
    // For nested condition type, evaluate sub-conditions recursively
    else
    {
        bool result = (condition.logic == "AND");
        std::vector<bool> results;
        for (const auto& subCond : condition.subConditions)
        {
            if (condition.logic == "AND")
            {
                result = result && evaluateCondition(bus, subCond);
            }
            else if (condition.logic == "OR")
            {
                result = result || evaluateCondition(bus, subCond);
            }
        }
        return result;
    }
    return true;
}

void StateMachineHandler::executeTransition()
{
    auto bus = sdbusplus::bus::new_default();

    // this loop iterates over each state value which can
    //  be achieved
    for (const State& stateValueTransition : states)
    {
        bool result = false;
        try
        {
            result = evaluateCondition(bus, stateValueTransition.conditions);
        }
        catch (const std::exception& e)
        {
            // set the fallback condition as we are getting error while
            // evaluating condition
            setPropertyValue(stateProperty, defaultState);
            log<level::ERR>(
                "Unable to evaluate condition",
                entry("OBJECT_PATH=%s",
                      stateValueTransition.conditions.object.c_str()),
                entry("INTERFACE=%s",
                      stateValueTransition.conditions.intf.c_str()),
                entry("PROPERTY=%s",
                      stateValueTransition.conditions.property.c_str()),
                entry("REASON=%s", e.what()));
            return;
        }

        // if evaluation is true we set the property and return
        if (result && getCurrState() != stateValueTransition.name)
        {
            setPropertyValue(stateProperty, stateValueTransition.name);
            doActions(bus, stateValueTransition.actions);
            return;
        }
    }
}

void StateMachineHandler::doActions(sdbusplus::bus::bus& bus,
                                    const std::vector<std::string>& actions)
{
    for (const auto& action : actions)
    {
        try
        {
            auto method = bus.new_method_call(SYSTEMD_SERVICE, SYSTEMD_OBJ_PATH,
                                              SYSTEMD_INTERFACE, "RestartUnit");
            method.append(action, "replace");

            bus.call_noreply(method);

            log<level::INFO>(
                std::format("Requested to start systemd service: '{}'", action)
                    .c_str());
        }
        catch (const sdbusplus::exception_t& e)
        {
            log<level::ERR>(
                std::format("Failed to queue service start '{}': {}", action,
                            e.what())
                    .c_str());
        }
    }
}

/** @brief Parsing JSON file  */
Json ConfigurableStateManager::parseConfigFile(const std::string& configFile)
{
    Json data;
    // check  json file
    std::ifstream errhandler_json_file(configFile);
    if (!errhandler_json_file.good())
    {
        errhandler_json_file.close();
        log<level::ERR>("Json  file  not found!",
                        entry("FILE_NAME=%s", configFile.c_str()));
        return data;
    }

    data = Json::parse(errhandler_json_file, nullptr, false);
    if (data.is_discarded())
    {
        log<level::ERR>("Corrupted Json file",
                        entry("FILE_NAME=%s", configFile.c_str()));
        return data;
    }
    return data;
}

Condition ConfigurableStateManager::parseCondition(const Json& conditionJson)
{
    Condition condition;

    // Check if this is a simple condition by looking for required fields
    if (conditionJson.contains("Object"))
    {
        // Parse as simple condition
        condition.service = conditionJson.value("Service", "");
        condition.object = conditionJson.value("Object", "");
        condition.intf = conditionJson.value("Intf", "");
        condition.property = conditionJson.value("Property", "");
        condition.value = conditionJson.value("Value", "");
    }
    else
    {
        // Parse as nested condition
        condition.logic = conditionJson.value("Logic", "");

        // Process nested conditions with "cond_" prefix
        for (const auto& [key, value] : conditionJson.items())
        {
            if (key.rfind("cond_", 0) == 0)
            {
                condition.subConditions.push_back(parseCondition(value));
            }
        }
    }

    // Throw exception if both object and subConditions are empty
    if (condition.object.empty() && condition.subConditions.empty())
    {
        throw std::runtime_error(
            "Invalid condition: both object path and sub-conditions are empty");
    }
    // Throw exception if there are multiple subConditions but no logic
    // specified
    if (condition.subConditions.size() > 1 && condition.logic.empty())
    {
        throw std::runtime_error(
            "Invalid condition: logic must be specified when multiple sub-conditions are present");
    }
    // Throw exception if condition has both simple condition fields and
    // sub-conditions
    if (!condition.object.empty() && !condition.subConditions.empty())
    {
        throw std::runtime_error(
            "Invalid condition: cannot mix simple and nested conditions");
    }
    // Throw exception if logic type is not "AND" or "OR"
    if (!condition.logic.empty() && condition.logic != "AND" &&
        condition.logic != "OR")
    {
        throw std::runtime_error("Unsupported logic gate used");
    }

    if (condition.logic.empty() && condition.subConditions.size() == 1)
    {
        condition.logic = "AND";
    }

    return condition;
}

} // namespace configurable_state_manager
////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Service Entry Point
 */
int main()
{
    log<level::INFO>("Creating Configurable State Manager connection");
    auto io = std::make_shared<boost::asio::io_context>();
    auto conn = std::make_shared<sdbusplus::asio::connection>(*io);

    // For now, we only have one instance of the configurable state manager
    auto objPathInst = std::string{CUSTOM_OBJPATH};

    // Add sdbusplus ObjectManager.
    sdbusplus::server::manager::manager objManager(*conn, objPathInst.c_str());
    configurable_state_manager::ConfigurableStateManager manager;
    conn->request_name(CUSTOM_BUSNAME);

    // Folder path to JSON files
    std::string folderPath = std::string{CUSTOM_FILEPATH};
    std::vector<std::string> jsonFiles;
    for (const auto& filePath : fs::directory_iterator(folderPath))
    {
        if (filePath.is_regular_file() &&
            filePath.path().extension() == ".json")
        {
            jsonFiles.push_back(filePath.path().string());
        }
    }

    // Sort the JSON file paths alphabetically
    std::sort(jsonFiles.begin(), jsonFiles.end());

    // Process JSON files in alphabetical order
    for (const auto& jsonFilePath : jsonFiles)
    {
        const std::string& configFile = jsonFilePath;
        Json data = manager.parseConfigFile(configFile);
        if (data.is_null() || data.is_discarded())
        {
            continue;
        }
        // debug logging for filename being parsed
        auto errStr1 = (boost::format("Filename is:%s") % configFile).str();
        log<level::DEBUG>(errStr1.c_str());
        try
        {
            // Extract the relevant data from the parsed JSON
            std::string interfaceName = data["InterfaceName"];
            std::string featureType = data["TypeInCategory"];
            std::string objToBeAdded = objPathInst + "/";
            // extract type from Feature Type
            // Find the last occurrence of '.'
            size_t lastDotPos = featureType.rfind('.');

            std::string extractedString;
            // Check if a dot was found
            if (lastDotPos != std::string::npos)
            {
                // Extract the substring after the last dot
                extractedString = featureType.substr(lastDotPos + 1);
            }
            else
            {
                extractedString = featureType;
            }
            objToBeAdded = objToBeAdded + extractedString;

            std::string stateProperty = data["State"]["State_property"];
            std::string defaultState = data["State"]["Default"];
            std::string errorState = "";

            std::vector<configurable_state_manager::State> states;
            // Extract states from JSON
            for (const auto& stateEntry : data["State"]["States"].items())
            {
                // std::string stateName = stateEntry.key();
                configurable_state_manager::State
                    state; // Create a State object
                // Extract state-specific data
                state.name = stateEntry.key();
                // Extract conditions
                state.conditions =
                    manager.parseCondition(stateEntry.value()["Conditions"]);
                // Extract actions
                state.actions = stateEntry.value().value(
                    "Actions", std::vector<std::string>());

                // Add the state to the states vector
                states.push_back(state);
            }

            if (interfaceName.find("FeatureReady") != std::string::npos)
            {
                errorState =
                    "xyz.openbmc_project.State.FeatureReady.States.Unknown";
                manager.featureEntities.push_back(
                    std::move(std::make_unique<
                              configurable_state_manager::CategoryFeatureReady>(
                        *conn, objToBeAdded.c_str(), interfaceName, featureType,
                        stateProperty, defaultState, errorState, states)));
            }
            else if (interfaceName.find("DeviceReady") != std::string::npos)
            {
                errorState =
                    "xyz.openbmc_project.State.DeviceReady.States.Unknown";
                manager.deviceEntities.push_back(
                    std::move(std::make_unique<
                              configurable_state_manager::CategoryDeviceReady>(
                        *conn, objToBeAdded.c_str(), interfaceName, featureType,
                        stateProperty, defaultState, errorState, states)));
            }
            else if (interfaceName.find("InterfaceReady") != std::string::npos)
            {
                errorState =
                    "xyz.openbmc_project.State.InterfaceReady.States.Unknown";
                manager.interfaceEntities.push_back(std::move(
                    std::make_unique<
                        configurable_state_manager::CategoryInterfaceReady>(
                        *conn, objToBeAdded.c_str(), interfaceName, featureType,
                        stateProperty, defaultState, errorState, states)));
            }
            else if (interfaceName.find("ServiceReady") != std::string::npos)
            {
                errorState =
                    "xyz.openbmc_project.State.ServiceReady.States.Unknown";
                manager.serviceEntities.push_back(
                    std::move(std::make_unique<
                              configurable_state_manager::CategoryServiceReady>(
                        *conn, objToBeAdded.c_str(), interfaceName, featureType,
                        stateProperty, defaultState, errorState, states)));
            }
            else if (interfaceName.find("State.Chassis") != std::string::npos)
            {
                errorState =
                    "xyz.openbmc_project.State.Chassis.PowerState.Unknown";
                manager.powerEntities.push_back(std::move(
                    std::make_unique<
                        configurable_state_manager::CategoryChassisPowerReady>(
                        *conn, objToBeAdded.c_str(), interfaceName, featureType,
                        stateProperty, defaultState, errorState, states)));
            }
        }
        catch (std::exception& e)
        {
            auto errStrPath =
                (boost::format("Corrupted Json file, Filename is:%s, [E]:%s") %
                 configFile % e.what())
                    .str();
            log<level::ERR>(errStrPath.c_str());

            // continue processing for next json file
            continue;
        }
    }
    // Start the Asio I/O service
    io->run();
    return 0;
}
