/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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
#pragma once
#include <sdbusplus/bus.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/server/manager.hpp>
#include "config.h"
#include <phosphor-logging/log.hpp>
#include "xyz/openbmc_project/State/DeviceReady/server.hpp"
#include "xyz/openbmc_project/State/FeatureReady/server.hpp"
#include "xyz/openbmc_project/State/InterfaceReady/server.hpp"
#include "xyz/openbmc_project/State/ServiceReady/server.hpp"
#include "xyz/openbmc_project/State/Chassis/server.hpp"
#include <nlohmann/json.hpp>
#include <boost/format.hpp>
#include <phosphor-logging/log.hpp>
#include "utils.hpp"
#include <iostream>
#include <variant>
using namespace phosphor::logging;
using Json = nlohmann::ordered_json;

//in case of local dependency for get/set on property use this cache
//to be used because get/set on same service gives deadlock
// local cache containg objectPath, propertyName combination
static std::unordered_map<std::string, std::string> localCache = {
    {"/xyz/openbmc_project/state/configurableStateManager/ChassisPower", "Unknown"}
};

namespace configurable_state_manager
{
using FeatureIntfInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::State::server::FeatureReady>;
using DeviceIntfInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::State::server::DeviceReady>;
using InterfaceIntfInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::State::server::InterfaceReady>;
using ServiceIntfInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::State::server::ServiceReady>;
using ChassisIntfInherit = sdbusplus::server::object::object<
    sdbusplus::xyz::openbmc_project::State::server::Chassis>;

// Define a visitor to convert the variant to a string
struct VariantToStringVisitor {
    std::string operator()(int value) const {
        return std::to_string(value);
    }

    std::string operator()(double value) const {
        return std::to_string(value);
    }

    std::string operator()(const std::string& value) const {
        return value;
    }

    std::string operator()(bool value) const {
        return value ? "true" : "false";
    }
};

// Define a structure for conditions
struct Condition {
    std::string intf;
    std::string property;
    std::string value;
    std::string logic;
};

// Define a structure for states
struct State {
    std::string name;
    std::vector<Condition> conditions;
    std::string logic;
};

class StateMachineHandler {
  public:
    std::string interfaceName;
    std::string featureType;
    std::unordered_map<std::string, std::vector<std::string>> servicesToBeMonitored;
    std::string stateProperty;
    std::string defaultState;
    std::string errorState;
    std::string objPathCreated;
    std::vector<State> states;
    // Constructor that takes the JSON configuration as input
    StateMachineHandler(const std::string& interfaceName,
                 const std::string& featureType,
                 const std::unordered_map<std::string, std::vector<std::string>>& servicesToBeMonitored,
                 const std::string& stateProperty,
                 const std::string& defaultState,
                 const std::string& errorState,
                 const char* objPathCreated,
                 const std::vector<State>& states) :
        interfaceName(interfaceName),
        featureType(featureType),
        servicesToBeMonitored(servicesToBeMonitored),
        stateProperty(stateProperty),
        defaultState(defaultState),
        errorState(errorState),
        objPathCreated(objPathCreated),
        states(states) 
    {}
    virtual ~StateMachineHandler() {}

    std::vector<std::unique_ptr<sdbusplus::bus::match::match>>
        eventHandlerMatcher;

    void executeTransition();
    bool any(const std::vector<bool>& bool_vector);
    bool all(const std::vector<bool>& bool_vector);
    virtual void setPropertyValue(const std::string& propertyName,
                               const std::string& val) = 0;
};

class CategoryFeatureReady : public FeatureIntfInherit, StateMachineHandler
{
  public:
    PropertiesVariant getPropertyValue(const std::string& stateProperty, const std::string& propertyValueString)
    {
        if(stateProperty == "State")
        {
            return convertStatesFromString(propertyValueString);
        }
        else
        {
            return convertFeatureTypesFromString(propertyValueString);
        }
    }

    void setPropertyValue(const std::string& stateProperty,
                            const std::string& val)
    {
        setPropertyByName(stateProperty, getPropertyValue(stateProperty, val));
    }

    CategoryFeatureReady(sdbusplus::bus::bus & bus, const char* objPath,
                 const std::string& interfaceName,
                 const std::string& featureType,
                 const std::unordered_map<std::string, std::vector<std::string>>& servicesToBeMonitored,
                 const std::string& stateProperty,
                 const std::string& defaultState,
                 const std::string& errorState,
                 const std::vector<State>& states) :
        FeatureIntfInherit(bus, objPath),
        StateMachineHandler(interfaceName, featureType, servicesToBeMonitored, stateProperty, defaultState, errorState, objPath, states)
    {
        //populate default state
        setPropertyValue(stateProperty, defaultState);
        //populate type
        setPropertyValue("FeatureType", featureType);

        try
        {
            //execute transition logic at startup
            //init for the category
            executeTransition();
        }
        catch(const std::exception& e)
        {
            auto errStrPath = (boost::format("CategoryFeatureReady : [E]:%s") % e.what()).str();
            log<level::ERR>(errStrPath.c_str());
        }

        for (const auto& interfaceEntry : servicesToBeMonitored)
        {
            const std::string& ifaceName = interfaceEntry.first;
            const std::vector<std::string>& objPaths = interfaceEntry.second;
        
            for (const std::string& objPath : objPaths)
            {
               auto matchPtr = std::make_unique<sdbusplus::bus::match::match>(
                    sdbusplus::bus::match::match(bus,
                    sdbusplus::bus::match::rules::propertiesChanged(
                    std::string(objPath), std::string(ifaceName)),
                    [&](sdbusplus::message::message& msg) 
                    {
                        try
                        {
                            // Execute the transition when properties change
                            executeTransition();
                            // for logging
                            std::string objectPathSender = msg.get_sender();
                            log<level::INFO>("Object path which calle this",
                                entry("OBJ_NAME=%s", objectPathSender.c_str()));
                        }
                        catch(const sdbusplus::exception::SdBusError& e)
                        {
                            log<level::ERR>("Unable to execute Transiton", entry("ERR=%s msg=", e.what()));
                        }
                    }));
                        
                eventHandlerMatcher.push_back(std::move(matchPtr));

                // create interface added matchPtr
                auto matchPtr2 = std::make_unique<sdbusplus::bus::match::match>(
                    sdbusplus::bus::match::match(bus,
                    sdbusplus::bus::match::rules::interfacesAdded() +
                    sdbusplus::bus::match::rules::argNpath(0, std::string(objPath)),
                    [&](sdbusplus::message::message& msg) 
                    {
                        std::map<std::string, std::map<std::string, std::variant<std::string>>> interfacesMap;
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
                                // for logging
                                std::string objectPathSender = msg.get_sender();
                                log<level::INFO>("Object path which called this",
                                    entry("OBJ_NAME=%s", objectPathSender.c_str()));
                            }
                            catch(const sdbusplus::exception::SdBusError& e)
                            {
                                log<level::ERR>("Unable to execute Transiton for    interface added matchPtr", entry("ERR=%s msg=", e. what()));
                            }
                        }
                    }));

                // insert interface added matchPtr         
                eventHandlerMatcher.push_back(std::move(matchPtr2));
            }
        }
    }
};

class CategoryServiceReady : public ServiceIntfInherit, StateMachineHandler
{
  public:
    PropertiesVariant getPropertyValue(const std::string& stateProperty, const std::string& propertyValueString)
    {
        if(stateProperty == "State")
        {
            return convertStatesFromString(propertyValueString);
        }
        else
        {
            return convertServiceTypesFromString(propertyValueString);
        }
    }

    void setPropertyValue(const std::string& stateProperty,
                            const std::string& val)
    {
        setPropertyByName(stateProperty, getPropertyValue(stateProperty, val));
    }

    CategoryServiceReady(sdbusplus::bus::bus& bus, const char* objPath,
                 const std::string& interfaceName,
                 const std::string& featureType,
                 const std::unordered_map<std::string, std::vector<std::string>>& servicesToBeMonitored,
                 const std::string& stateProperty,
                 const std::string& defaultState,
                 const std::string& errorState,
                 const std::vector<State>& states) :
        ServiceIntfInherit(bus, objPath),
        StateMachineHandler(interfaceName, featureType, servicesToBeMonitored, stateProperty, defaultState, errorState, objPath, states)
    {
        //populate default state
        setPropertyValue(stateProperty, defaultState);
        //populate type
        setPropertyValue("ServiceType", featureType);
        
        try
        {
            //execute transition logic at startup
            //init for the category
            executeTransition();
        }
        catch(const std::exception& e)
        {
            auto errStrPath = (boost::format("CategoryServiceReady : [E]:%s") % e.what()).str();
            log<level::ERR>(errStrPath.c_str());
        }

        for (const auto& interfaceEntry : servicesToBeMonitored)
        {
            const std::string& ifaceName = interfaceEntry.first;
            const std::vector<std::string>& objPaths = interfaceEntry.second;
        
            for (const std::string& objPath : objPaths)
            {
               auto matchPtr = std::make_unique<sdbusplus::bus::match::match>(
                    sdbusplus::bus::match::match(bus,
                    sdbusplus::bus::match::rules::propertiesChanged(
                    std::string(objPath), std::string(ifaceName)),
                    [&](sdbusplus::message::message& msg) 
                    {
                        try
                        {
                            // Execute the transition when properties change
                            executeTransition();
                            // for logging
                            std::string objectPathSender = msg.get_sender();
                            log<level::INFO>("Object path which calle this",
                                entry("OBJ_NAME=%s", objectPathSender.c_str()));
                        }
                        catch(const sdbusplus::exception::SdBusError& e)
                        {
                            log<level::ERR>("Unable to execute Transiton", entry("ERR=%s msg=", e.what()));
                        }
                    }));
                        
                eventHandlerMatcher.push_back(std::move(matchPtr));

                // create interface added matchPtr
                auto matchPtr2 = std::make_unique<sdbusplus::bus::match::match>(
                    sdbusplus::bus::match::match(bus,
                    sdbusplus::bus::match::rules::interfacesAdded() +
                    sdbusplus::bus::match::rules::argNpath(0, std::string(objPath)),
                    [&](sdbusplus::message::message& msg) 
                    {
                        std::map<std::string, std::map<std::string, std::variant<std::string>>> interfacesMap;
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
                                // for logging
                                std::string objectPathSender = msg.get_sender();
                                log<level::INFO>("Object path which called this",
                                    entry("OBJ_NAME=%s", objectPathSender.c_str()));
                            }
                            catch(const sdbusplus::exception::SdBusError& e)
                            {
                                log<level::ERR>("Unable to execute Transiton for    interface added matchPtr", entry("ERR=%s msg=", e. what()));
                            }
                        }
                    }));

                // insert interface added matchPtr         
                eventHandlerMatcher.push_back(std::move(matchPtr2));
            }
        }
    }
};

class CategoryInterfaceReady : public InterfaceIntfInherit, StateMachineHandler
{
  public:
    PropertiesVariant getPropertyValue(const std::string& stateProperty, const std::string& propertyValueString)
    {
        if(stateProperty == "State")
        {
            return convertStatesFromString(propertyValueString);
        }
        else
        {
            return convertInterfaceTypesFromString(propertyValueString);
        }
    }

    void setPropertyValue(const std::string& stateProperty,
                            const std::string& val)
    {
        setPropertyByName(stateProperty, getPropertyValue(stateProperty, val));
    }

    CategoryInterfaceReady(sdbusplus::bus_t& bus, const char* objPath,
                 const std::string& interfaceName,
                 const std::string& featureType,
                 const std::unordered_map<std::string, std::vector<std::string>>& servicesToBeMonitored,
                 const std::string& stateProperty,
                 const std::string& defaultState,
                 const std::string& errorState,
                 const std::vector<State>& states) :
        InterfaceIntfInherit(bus, objPath),
        StateMachineHandler(interfaceName, featureType, servicesToBeMonitored, stateProperty, defaultState, errorState, objPath, states)
    {
        //populate default state
        setPropertyValue(stateProperty, defaultState);
        //populate type
        setPropertyValue("InterfaceType", featureType);
        
        try
        {
            //execute transition logic at startup
            //init for the category
            executeTransition();
        }
        catch(const std::exception& e)
        {
            auto errStrPath = (boost::format("CategoryInterfaceReady : [E]:%s") % e.what()).str();
            log<level::ERR>(errStrPath.c_str());
        }

        for (const auto& interfaceEntry : servicesToBeMonitored)
        {
            const std::string& ifaceName = interfaceEntry.first;
            const std::vector<std::string>& objPaths = interfaceEntry.second;
        
            for (const std::string& objPath : objPaths)
            {
               auto matchPtr = std::make_unique<sdbusplus::bus::match::match>(
                    sdbusplus::bus::match::match(bus,
                    sdbusplus::bus::match::rules::propertiesChanged(
                    std::string(objPath), std::string(ifaceName)),
                    [&](sdbusplus::message::message& msg) 
                    {
                        try
                        {
                            // Execute the transition when properties change
                            executeTransition();
                            // for logging
                            std::string objectPathSender = msg.get_sender();
                            log<level::INFO>("Object path which calle this",
                                entry("OBJ_NAME=%s", objectPathSender.c_str()));
                        }
                        catch(const sdbusplus::exception::SdBusError& e)
                        {
                            log<level::ERR>("Unable to execute Transiton", entry("ERR=%s msg=", e.what()));
                        }
                    }));
                        
                eventHandlerMatcher.push_back(std::move(matchPtr));

                // create interface added matchPtr
                auto matchPtr2 = std::make_unique<sdbusplus::bus::match::match>(
                    sdbusplus::bus::match::match(bus,
                    sdbusplus::bus::match::rules::interfacesAdded() +
                    sdbusplus::bus::match::rules::argNpath(0, std::string(objPath)),
                    [&](sdbusplus::message::message& msg) 
                    {
                        std::map<std::string, std::map<std::string, std::variant<std::string>>> interfacesMap;
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
                                // for logging
                                std::string objectPathSender = msg.get_sender();
                                log<level::INFO>("Object path which called this",
                                    entry("OBJ_NAME=%s", objectPathSender.c_str()));
                            }
                            catch(const sdbusplus::exception::SdBusError& e)
                            {
                                log<level::ERR>("Unable to execute Transiton for    interface added matchPtr", entry("ERR=%s msg=", e. what()));
                            }
                        }
                    }));

                // insert interface added matchPtr         
                eventHandlerMatcher.push_back(std::move(matchPtr2));
            }
        }
    }
};

class CategoryDeviceReady : public DeviceIntfInherit, StateMachineHandler
{
  public:
    PropertiesVariant getPropertyValue(const std::string& stateProperty, const std::string& propertyValueString)
    {
        if(stateProperty == "State")
        {
            return convertStatesFromString(propertyValueString);
        }
        else
        {
            return convertDeviceTypesFromString(propertyValueString);
        }
    }

    void setPropertyValue(const std::string& stateProperty,
                            const std::string& val)
    {
        setPropertyByName(stateProperty, getPropertyValue(stateProperty, val));
    }

    CategoryDeviceReady(sdbusplus::bus_t& bus, const char* objPath,
                 const std::string& interfaceName,
                 const std::string& featureType,
                 const std::unordered_map<std::string, std::vector<std::string>>& servicesToBeMonitored,
                 const std::string& stateProperty,
                 const std::string& defaultState,
                 const std::string& errorState,
                 const std::vector<State>& states) :
        DeviceIntfInherit(bus, objPath),
        StateMachineHandler(interfaceName, featureType, servicesToBeMonitored, stateProperty, defaultState, errorState, objPath, states)
    {
        //populate default state
        setPropertyValue(stateProperty, defaultState);
        //populate type
        setPropertyValue("DeviceType", featureType);
        
        try
        {
            //execute transition logic at startup
            //init for the category
            executeTransition();
        }
        catch(const std::exception& e)
        {
            auto errStrPath = (boost::format("CategoryDeviceReady : [E]:%s") % e.what()).str();
            log<level::ERR>(errStrPath.c_str());
        }

        for (const auto& interfaceEntry : servicesToBeMonitored)
        {
            const std::string& ifaceName = interfaceEntry.first;
            const std::vector<std::string>& objPaths = interfaceEntry.second;
        
            for (const std::string& objPath : objPaths)
            {
               auto matchPtr = std::make_unique<sdbusplus::bus::match::match>(
                    sdbusplus::bus::match::match(bus,
                    sdbusplus::bus::match::rules::propertiesChanged(
                    std::string(objPath), std::string(ifaceName)),
                    [&](sdbusplus::message::message& msg) 
                    {
                        try
                        {
                            // Execute the transition when properties change
                            executeTransition();
                            // for logging
                            std::string objectPathSender = msg.get_sender();
                            log<level::INFO>("Object path which calle this",
                                entry("OBJ_NAME=%s", objectPathSender.c_str()));
                        }
                        catch(const sdbusplus::exception::SdBusError& e)
                        {
                            log<level::ERR>("Unable to execute Transiton", entry("ERR=%s msg=", e.what()));
                        }
                    }));
                        
                eventHandlerMatcher.push_back(std::move(matchPtr));

                // create interface added matchPtr
                auto matchPtr2 = std::make_unique<sdbusplus::bus::match::match>(
                    sdbusplus::bus::match::match(bus,
                    sdbusplus::bus::match::rules::interfacesAdded() +
                    sdbusplus::bus::match::rules::argNpath(0, std::string(objPath)),
                    [&](sdbusplus::message::message& msg) 
                    {
                        std::map<std::string, std::map<std::string, std::variant<std::string>>> interfacesMap;
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
                                // for logging
                                std::string objectPathSender = msg.get_sender();
                                log<level::INFO>("Object path which called this",
                                    entry("OBJ_NAME=%s", objectPathSender.c_str()));
                            }
                            catch(const sdbusplus::exception::SdBusError& e)
                            {
                                log<level::ERR>("Unable to execute Transiton for    interface added matchPtr", entry("ERR=%s msg=", e. what()));
                            }
                        }
                    }));

                // insert interface added matchPtr         
                eventHandlerMatcher.push_back(std::move(matchPtr2));
            }
        }
    }
};

class CategoryChassisPowerReady : public ChassisIntfInherit, StateMachineHandler
{
  public:
    PropertiesVariant getPropertyValue(const std::string& propertyValueString)
    {
        return convertPowerStateFromString(propertyValueString);
    }

    void setPropertyValue(const std::string& stateProperty,
                            const std::string& val)
    {
        setPropertyByName(stateProperty, getPropertyValue(val));
        // update local cache also
        localCache[this->objPathCreated] = val;
    }

    CategoryChassisPowerReady(sdbusplus::bus_t& bus, const char* objPath,
                 const std::string& interfaceName,
                 const std::string& featureType,
                 const std::unordered_map<std::string, std::vector<std::string>>& servicesToBeMonitored,
                 const std::string& stateProperty,
                 const std::string& defaultState,
                 const std::string& errorState,
                 const std::vector<State>& states) :
        ChassisIntfInherit(bus, objPath),
        StateMachineHandler(interfaceName, featureType, servicesToBeMonitored, stateProperty, defaultState, errorState, objPath, states)
    {
        //populate default value of state
        setPropertyValue(stateProperty, defaultState);
        
        try
        {
            //execute transition logic at startup
            //init for the category
            executeTransition();
        }
        catch(const std::exception& e)
        {
            auto errStrPath = (boost::format("CategoryChassisPowerReady : [E]:%s") % e.what()).str();
            log<level::ERR>(errStrPath.c_str());
        }

        for (const auto& interfaceEntry : servicesToBeMonitored)
        {
            const std::string& ifaceName = interfaceEntry.first;
            const std::vector<std::string>& objPaths = interfaceEntry.second;
        
            for (const std::string& objPath : objPaths)
            {
               // create propertiesChange matchptr
               auto matchPtr = std::make_unique<sdbusplus::bus::match::match>(
                    sdbusplus::bus::match::match(bus,
                    sdbusplus::bus::match::rules::propertiesChanged(
                    std::string(objPath), std::string(ifaceName)),
                    [&](sdbusplus::message::message& msg) 
                    {
                        try
                        {
                            // Execute the transition when properties change
                            executeTransition();
                            // for logging
                            std::string objectPathSender = msg.get_sender();
                            log<level::INFO>("Object path which calle this",
                                entry("OBJ_NAME=%s", objectPathSender.c_str()));
                        }
                        catch(const sdbusplus::exception::SdBusError& e)
                        {
                            log<level::ERR>("Unable to execute Transiton for property change matchPtr", entry("ERR=%s msg=", e.what()));
                        }
                    }));

                // insert propertiesChange matchPtr         
                eventHandlerMatcher.push_back(std::move(matchPtr));

                // create interface added matchPtr
                auto matchPtr2 = std::make_unique<sdbusplus::bus::match::match>(
                    sdbusplus::bus::match::match(bus,
                    sdbusplus::bus::match::rules::interfacesAdded() +
                    sdbusplus::bus::match::rules::argNpath(0, std::string(objPath)),
                    [&](sdbusplus::message::message& msg) 
                    {
                        std::map<std::string, std::map<std::string, std::variant<std::string>>> interfacesMap;
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
                                // for logging
                                std::string objectPathSender = msg.get_sender();
                                log<level::INFO>("Object path which called this",
                                    entry("OBJ_NAME=%s", objectPathSender.c_str()));
                            }
                            catch(const sdbusplus::exception::SdBusError& e)
                            {
                                log<level::ERR>("Unable to execute Transiton for    interface added matchPtr", entry("ERR=%s msg=", e. what()));
                            }
                        }
                    }));

                // insert interfacesAdded matchPtr         
                eventHandlerMatcher.push_back(std::move(matchPtr2));
            }
        }
    }
};

class ConfigurableStateManager
{
  public:
    // Constructor
    ConfigurableStateManager()
    {}
    // Destructor
    ~ConfigurableStateManager()
    {}

    /** @brief Parse JSON file  */
    Json parseConfigFile(const std::string& configFile);

    // Declare vectors to hold the different entity objects
    std::vector<std::unique_ptr<CategoryFeatureReady>> featureEntities;
    std::vector<std::unique_ptr<CategoryDeviceReady>> deviceEntities;
    std::vector<std::unique_ptr<CategoryInterfaceReady>> interfaceEntities;
    std::vector<std::unique_ptr<CategoryServiceReady>> serviceEntities;
    std::vector<std::unique_ptr<CategoryChassisPowerReady>> powerEntities;
};
} // namespace configurable_state_manager

