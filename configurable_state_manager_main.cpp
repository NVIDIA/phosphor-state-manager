#include <sdbusplus/asio/object_server.hpp> // Include the asio/object_server header
#include <sdbusplus/asio/connection.hpp> // Include the asio/connection header
#include <sdbusplus/asio/property.hpp> // Include the asio/property header
#include "config.h"
#include "configurable_state_manager.hpp"
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/manager.hpp>
#include <sdbusplus/server.hpp>
#include <phosphor-logging/log.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <variant>
#include <nlohmann/json.hpp>
#include <boost/format.hpp>
#include "utils.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace phosphor::logging;

namespace configurable_state_manager
{

bool StateMachineHandler::any(const std::vector<bool>& bool_vector) {
    for (bool value : bool_vector) {
        if (value) {
            return true;
        }
    }
    return false;
}

bool StateMachineHandler::all(const std::vector<bool>& bool_vector) {
    for (bool value : bool_vector) {
        if (!value) {
            return false;
        }
    }
    return true;
}

void StateMachineHandler::executeTransition()
{
    auto bus = sdbusplus::bus::new_default();

    //this loop iterates over each state value which can
    // be achieved
    for (const State& stateValueTransition : states)  
    {
        std::string stateValue = stateValueTransition.name;
        //const State& stateValueTransition = stateEntry.second;
        std::string stateValueLogic = stateValueTransition.logic;
        std::vector<bool> evalConditions;

        // booolean variable indicating to jump to next iteration
        // state evaluation
        bool jumpToNextState = false;

        // Process conditions to be met to attain the state
        for (const Condition& condition : stateValueTransition.conditions) 
        {
            std::vector<bool> evalConditionLoop;
            // Iterate over objects associated with condition.intf
            for (const std::string& objectPath : servicesToBeMonitored[condition.intf]) 
            {
                //variable to hold output for getProperty
                phosphor::state::manager::utils::PropertyValue tmp;
                try
                {
                    if(objectPath.find("ChassisPower") != std::string::npos)
                    {
                        tmp = chassisCurrentPowerState;
                    }
                    else
                    {
                        tmp = phosphor::state::manager::utils::getProperty(bus, objectPath, condition.intf, condition.property);
                    }
                }
                catch(const std::exception& e)
                {
                    auto errStrPath = (boost::format("Got error with getProperty() for combination objectPath::%s, interface::%s, property::%s, with exception:: [E]:%s. \n Skipping present state evaluation and moving to next state evaluation.") % objectPath % condition.intf % condition.property % e.what()).str();
                    log<level::ERR>(errStrPath.c_str());

                    //when error occurs we do not throw runtime error
                    //log the error and try to evaulate next state evaluation
                    //so jumping to next iteration of outermost loop.
                    jumpToNextState = true;
                    break;
                }

                std::string reqValue;

                if (std::holds_alternative<int>(tmp)) 
                {
                    int intValue = std::get<int>(tmp);
                    reqValue = std::to_string(intValue);
                } 
                /*else if (std::holds_alternative<double>(tmp)) 
                {
                    double doubleValue = std::get<double>(tmp);
                    reqValue = std::to_string(doubleValue);
                } */
                else if (std::holds_alternative<std::string>(tmp)) 
                {
                    reqValue = std::get<std::string>(tmp);
                } 
                else if (std::holds_alternative<bool>(tmp)) 
                {
                    bool boolValue = std::get<bool>(tmp);
                    reqValue = boolValue ? "true" : "false";
                } 
                /*else if (std::holds_alternative<const char*>(tmp)) 
                {
                    const char* charPtrValue = std::get<const char*>(tmp);
                    reqValue = charPtrValue;
                } */
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

            if (jumpToNextState)
            {
                break;
            }

            if (condition.logic.compare("AND") == 0)
            {
                evalConditions.push_back(all(evalConditionLoop));
            }
            else if (condition.logic.compare("OR") == 0)
            {
                evalConditions.push_back(any(evalConditionLoop));
            }
            else
            {
                // if no logic is present means only single entry
                evalConditions.push_back(evalConditionLoop[0]);
            }
        }

        if (jumpToNextState)
        {
            jumpToNextState = false;
            continue;
        }

        // final evaluation of all condition boolean results for a particular state value
        bool stateConditionsResult = false; 
        if (stateValueTransition.logic.compare("AND") == 0)
        {
            stateConditionsResult = all(evalConditions);
        }
        else if (stateValueTransition.logic.compare("OR") == 0)
        {
            stateConditionsResult = any(evalConditions);
        }
        else
        {
            // if no logic is present means only one condition was there
            stateConditionsResult = evalConditions[0];
        }

        // if evaluation is true we set the property and return
        if(stateConditionsResult)
        {
            setPropertyValue(stateProperty, stateValue);
            if(stateValue.find("xyz.openbmc_project.State.Chassis.PowerState") != std::string::npos)
            {
                chassisCurrentPowerState = stateValue;
            }
            return;
        }
    }
    log<level::ERR>("if we reach here means state evaluation of not a single state value was true, this may happened either json file is corrupted or some interfaces are not available which we are monitoring, -- Setting value with ConditionsFallback value --");
    setPropertyValue(stateProperty, conditionsFallbackState);
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
        //throw std::runtime_error("Config failed");
        isFileGood = false;
        return data;
    }

    data = Json::parse(errhandler_json_file, nullptr, false);
    if (data.is_discarded())
    {
        log<level::ERR>("Corrupted Json file",
                        entry("FILE_NAME=%s", configFile.c_str()));
        //throw std::runtime_error("Config failed");
        isFileGood = false;
    }
    return data;
}

} // configurable_state_manager
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
    std::string folderPath = "/usr/share/configurable-state-manager";
    std::vector<std::string> jsonFiles;
    for (const auto& filePath : fs::directory_iterator(folderPath)) 
    {
        if (filePath.is_regular_file() && filePath.path().extension() == ".json") 
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
        isFileGood = true;
        json data = manager.parseConfigFile(configFile);
        if(!isFileGood)
        {
            continue;
        }
        // debug logging for filename being parsed
        auto errStr1 = (boost::format("Filename is:%s")
                         % configFile).str();
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
            
            // Check if a dot was found
            if (lastDotPos != std::string::npos) 
            {
                // Extract the substring after the last dot
                std::string extractedString = featureType.substr(lastDotPos+ 1);
                objToBeAdded = objToBeAdded + extractedString;
            }
            else
            {
                objToBeAdded = objToBeAdded + featureType;
            }
            std::unordered_map<std::string,        
             std::vector<std::string>>servicesToBeMonitored = data      ["ServicesToBeMonitored"];
            std::string stateProperty = data["State"]["State_property"];
            std::string defaultState = data["State"]["Default"];
            std::string conditionsFallbackState = data["State"]["ConditionsFallback"];
            
            std::vector<configurable_state_manager::State> states;
            // Extract states from JSON
            for (const auto& stateEntry : data["State"]["States"].items()) 
            {
                //std::string stateName = stateEntry.key();
                configurable_state_manager::State state; // Create a State object
                // Extract state-specific data
                state.name = stateEntry.key();
                // optional field
                state.logic = stateEntry.value().value("Logic", "");        
                
                // Extract conditions
                for (const auto& conditionEntry : stateEntry.value()   ["Conditions"].items()) 
                {
                    configurable_state_manager::Condition condition;
                    condition.intf = conditionEntry.key();
                    condition.property = conditionEntry.value()["Property"];
                    condition.value = conditionEntry.value()["Value"];
                    // optional field
                    condition.logic = conditionEntry.value().value("Logic","");
                    state.conditions.push_back(condition);
                }
                // Add the state to the states vector
                states.push_back(state);
            }
            if (interfaceName.find("FeatureReady") != std::string::npos) 
            {
                manager.featureEntities.push_back(std::move(std::make_unique<configurable_state_manager::CategoryFeatureReady>(*conn, objToBeAdded.c_str(), interfaceName,featureType, servicesToBeMonitored, stateProperty,defaultState, conditionsFallbackState, states)));
            }
            else if (interfaceName.find("DeviceReady") != std::string::npos)
            {
                manager.deviceEntities.push_back(std::move(std::make_unique<configurable_state_manager::CategoryDeviceReady>(*conn, objToBeAdded.c_str(), interfaceName,featureType, servicesToBeMonitored, stateProperty,defaultState, conditionsFallbackState, states)));
            }
            else if (interfaceName.find("InterfaceReady") !=std::string::npos)
            {
                manager.interfaceEntities.push_back(std::move(std::make_unique<configurable_state_manager::CategoryInterfaceReady>(*conn, objToBeAdded.c_str(), interfaceName,featureType, servicesToBeMonitored, stateProperty,defaultState, conditionsFallbackState, states)));
            }
            else if (interfaceName.find("ServiceReady") !=std::string::npos)
            {
                manager.serviceEntities.push_back(std::move(std::make_unique<configurable_state_manager::CategoryServiceReady>(*conn, objToBeAdded.c_str(), interfaceName,featureType, servicesToBeMonitored, stateProperty,defaultState, conditionsFallbackState, states)));
            }
            else if (interfaceName.find("State.Chassis") !=std::string::npos)
            {
                manager.powerEntities.push_back(std::move(std::make_unique<configurable_state_manager::CategoryChassisPowerReady>(*conn, objToBeAdded.c_str(), interfaceName,featureType, servicesToBeMonitored, stateProperty,defaultState, conditionsFallbackState, states)));
            }
        }
        catch (std::exception& e)
        {
            auto errStrPath = (boost::format("Corrupted Json file, Filename is:%s, [E]:%s") % configFile % e.what()).str();
            log<level::ERR>(errStrPath.c_str());
            
            // continue processing for next json file
            continue;
        }
    } 
    // Start the Asio I/O service
    io->run();
    return 0;
}