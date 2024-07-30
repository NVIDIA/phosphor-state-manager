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
#include <sdbusplus/server.hpp>
#include <sdbusplus/server/manager.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <variant>
#include <vector>

namespace fs = std::filesystem;
using namespace phosphor::logging;

namespace configurable_state_manager
{

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

void StateMachineHandler::executeTransition()
{
    auto bus = sdbusplus::bus::new_default();

    // this loop iterates over each state value which can
    //  be achieved
    for (const State& stateValueTransition : states)
    {
        std::string stateValue = stateValueTransition.name;
        std::string stateValueLogic = stateValueTransition.logic;
        std::vector<bool> evalConditions;

        // Process conditions to be met to attain the state
        for (const Condition& condition : stateValueTransition.conditions)
        {
            std::vector<bool> evalConditionLoop;
            // Iterate over objects associated with condition.intf
            for (const std::string& objectPath :
                 servicesToBeMonitored[condition.intf])
            {
                // variable to hold output for getProperty
                phosphor::state::manager::utils::PropertyValue tmp;
                try
                {
                    // find the service name containing object, intf
                    std::string service =
                        phosphor::state::manager::utils::getService(
                            bus, objectPath, condition.intf);

                    // if service is empty set unknown state and return
                    if (service.empty())
                    {
                        log<level::ERR>("Unable to fetch service name, setting state as default state");
                        setPropertyValue(stateProperty, defaultState);
                        return;
                    }

                    auto errStrPatht =
                        (boost::format("service name fetched::%s ") % service)
                            .str();
                    log<level::ERR>(errStrPatht.c_str());
                    // if the service hosting the object is csm
                    // look in local cache
                    if (service.find("ConfigurableStateManager") !=
                        std::string::npos)
                    {
                        // if property hosted on same service use local cache
                        // this is kind of local get operation
                        tmp = localCache[objectPath];
                    }
                    else
                    {
                        tmp = phosphor::state::manager::utils::getPropertyV2(
                            bus, objectPath, condition.intf,
                            condition.property);
                    }
                }
                catch (const std::exception& e)
                {
                    auto errStrPath =
                        (boost::format(
                             "Got error with getProperty() for combination objectPath::%s, interface::%s, property::%s, with exception:: [E]:%s, hence setting state as default state") %
                         objectPath % condition.intf % condition.property %
                         e.what())
                            .str();
                    log<level::ERR>(errStrPath.c_str());

                    // set the fallback condition as we are getting error while
                    // evaluating condition
                    setPropertyValue(stateProperty, defaultState);
                    return;
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

                int res = condition.value.compare(reqValue);
                if (res == 0)
                {
                    evalConditionLoop.push_back(true);
                }
                else
                {
                    evalConditionLoop.push_back(false);
                }
            }

            if (condition.logic.compare("AND") == 0)
            {
                evalConditions.push_back(all(evalConditionLoop));
            }
            else if (condition.logic.compare("OR") == 0)
            {
                evalConditions.push_back(any(evalConditionLoop));
            }
            else if (condition.logic.empty())
            {
                // if no logic is present means only single entry
                evalConditions.push_back(evalConditionLoop[0]);
            }
            else
            {
                // other cases of not supported logics
                log<level::ERR>("Unsupported logic gate used, hence setting state as default state");
                // set state to unknown as feature evaluation got error
                setPropertyValue(stateProperty, defaultState);
                return;
            }
        }

        // final evaluation of all condition boolean results for a particular
        // state value
        bool stateConditionsResult = false;
        if (stateValueTransition.logic.compare("AND") == 0)
        {
            stateConditionsResult = all(evalConditions);
        }
        else if (stateValueTransition.logic.compare("OR") == 0)
        {
            stateConditionsResult = any(evalConditions);
        }
        else if (stateValueTransition.logic.empty())
        {
            // if no logic is present means only one condition was there
            stateConditionsResult = evalConditions[0];
        }
        else
        {
            // other cases of not supported logics
            log<level::ERR>("Unsupported logic gate used");
            return;
        }

        // if evaluation is true we set the property and return
        if (stateConditionsResult)
        {
            setPropertyValue(stateProperty, stateValue);
            return;
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

            std::unordered_map<std::string, std::vector<std::string>>
                servicesToBeMonitored = data["ServicesToBeMonitored"];
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
                // optional field
                state.logic = stateEntry.value().value("Logic", "");

                // Extract conditions
                for (const auto& conditionEntry :
                     stateEntry.value()["Conditions"].items())
                {
                    configurable_state_manager::Condition condition;
                    condition.intf = conditionEntry.key();
                    condition.property = conditionEntry.value()["Property"];
                    condition.value = conditionEntry.value()["Value"];
                    // optional field
                    condition.logic = conditionEntry.value().value("Logic", "");
                    state.conditions.push_back(condition);
                }
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
                        servicesToBeMonitored, stateProperty, defaultState,
                        errorState, states)));
            }
            else if (interfaceName.find("DeviceReady") != std::string::npos)
            {
                errorState =
                    "xyz.openbmc_project.State.DeviceReady.States.Unknown";
                manager.deviceEntities.push_back(
                    std::move(std::make_unique<
                              configurable_state_manager::CategoryDeviceReady>(
                        *conn, objToBeAdded.c_str(), interfaceName, featureType,
                        servicesToBeMonitored, stateProperty, defaultState,
                        errorState, states)));
            }
            else if (interfaceName.find("InterfaceReady") != std::string::npos)
            {
                errorState =
                    "xyz.openbmc_project.State.InterfaceReady.States.Unknown";
                manager.interfaceEntities.push_back(std::move(
                    std::make_unique<
                        configurable_state_manager::CategoryInterfaceReady>(
                        *conn, objToBeAdded.c_str(), interfaceName, featureType,
                        servicesToBeMonitored, stateProperty, defaultState,
                        errorState, states)));
            }
            else if (interfaceName.find("ServiceReady") != std::string::npos)
            {
                errorState =
                    "xyz.openbmc_project.State.ServiceReady.States.Unknown";
                manager.serviceEntities.push_back(
                    std::move(std::make_unique<
                              configurable_state_manager::CategoryServiceReady>(
                        *conn, objToBeAdded.c_str(), interfaceName, featureType,
                        servicesToBeMonitored, stateProperty, defaultState,
                        errorState, states)));
            }
            else if (interfaceName.find("State.Chassis") != std::string::npos)
            {
                errorState =
                    "xyz.openbmc_project.State.Chassis.PowerState.Unknown";
                manager.powerEntities.push_back(std::move(
                    std::make_unique<
                        configurable_state_manager::CategoryChassisPowerReady>(
                        *conn, objToBeAdded.c_str(), interfaceName, featureType,
                        servicesToBeMonitored, stateProperty, defaultState,
                        errorState, states)));
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
