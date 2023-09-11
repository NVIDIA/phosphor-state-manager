# Configurable State Manager (CSM) - A New Service For State Management in OpenBmc

Author:
 Rajat Jain (rajatj@nvidia.com)

Primary assignee:
 Rajat Jain (rajatj@nvidia.com)

Other contributors:
 Deepak Kodihalli (dkodihalli@nvidia.com)
 Shakeeb Pasha (spasha@nvidia.com) 

Created:
 October 17, 2023

## Contents
- Problem Description
- Background and References
- Requirements
- Proposed Design
- Categories Defined for state management
- Preset Dbus Tree for CSM service
- Understanding JSON configuration File Structure
- Flowchart of CSM
- End to End flow of CSM service block diagram for Telemetry Ready use case
- Special scenarios Handled
- Testing

## Problem Description
Till now there was no single service dedicated for state management in OpenBmc. Reporting of states is necessary for other openbmc services as well as redfish clients because their behaviour depends on state transitions.

## Background and References
There are states hosted by different services (e.g HMCReady by GpuMgr) mostly with OperationalStatus Dbus api. There is no single framework present which we can leverage and makes its easy to host state machine with well defined Dbus API's across categories.

So new service Configurable state manager was proposed which will be a centre point for hosting different state machines across various categories with well defined Dbus API's for each category. It will act as a manager responsible for handling all types of state machines.

For details about OpenBmc State Management - refer [1]
For details about Vulcan Power Handling, HMC Readiness, Sub services (GpuMgr,  Sensor server etc) handling through Configurable State Manager - refer [2]  

### References
[1] https://nvidia.sharepoint.com/:w:/r/sites/ServerPlatformSoftware/_layouts/15/Doc.aspx?sourcedoc=%7BC3A1B596-70B8-45D8-BBA1-D392A66EBA8E%7D&file=State%20Management%20in%20OpenBMC.docx&action=default&mobileredirect=true
[2] https://docs.google.com/document/d/1N8vbaIeEd-fOyZtvgWspA-41h6Tb3Jg8cLAeBNxYm4s/edit

## Requirements
- Create a new service configurable state manager.
- Create new Dbus API for each category of state machine defined in [1]. 
- Creating a new state machine should be as easy as adding new json file.
- Functionalities like reporting/fetching of state both on dbus and out of band redfish, signaling a state change, handling retires and errors on states, handling dependencies between states from different categories etc.
- States may be important throughout runtime or at bootup.

## Proposed Design
- Creating a new configurable state manager (csm) will be a single thread service based on phosphor-dbus-interface library. 
- Start/Stop of configurable state manager service is managed by systemd. Service name is "xyz.openbmc_project.State.ConfigurableStateManager"
- csm service is packed with Phosphor state manager.
- Transition of states will be through a data driven approach using json files.
- Each json file will correspond to a particular use case for which we want to report states. Json will contain all information about category type, object name to be created, default state, transition logic etc. We will discuss json structure in detail in later part of doc.
- Provide the framework for state management. Creating a new state machine for a particular use case should be as easy as adding a new json file.

### Categories Defined for state management
There are 5 categories defined in document [1].
- **Chassis/Host/BMC Power** :- e.g. Power off, standby power, DC power 
  - DBus API defined:
    - https://gitlab-master.nvidia.com/dgx/bmc/phosphor-dbus-interfaces/-/blob/develop/yaml/xyz/openbmc_project/State/Chassis.interface.yaml
    - https://gitlab-master.nvidia.com/dgx/bmc/phosphor-dbus-interfaces/-/blob/develop/yaml/xyz/openbmc_project/State/Host.interface.yaml
- **Devices** :- e.g.  OpenBMC state, ERoT state, FPGA state.
  - Dbus API defined:
    - https://gitlab-master.nvidia.com/dgx/bmc/phosphor-dbus-interfaces/-/blob/develop/yaml/xyz/openbmc_project/State/DeviceReady.interface.yaml
- **HMC/BMC Interfaces/Busses** :- e.g. PCIe state, I2C state, SSIF state, Ethernet state.
  - Dbus API defined:
    - https://gitlab-master.nvidia.com/dgx/bmc/phosphor-dbus-interfaces/-/blob/develop/yaml/xyz/openbmc_project/State/InterfaceReady.interface.yaml   
- **Services** :- e.g. OpenBMC Core and Platform PICs 
  - Dbus API defined:
    - https://gitlab-master.nvidia.com/dgx/bmc/phosphor-dbus-interfaces/-/blob/develop/yaml/xyz/openbmc_project/State/ServiceReady.interface.yaml
- **Feature** :- e.g. Telemetry Ready, FW Update ready, Redfish/HMC/BMC Ready 
  - Dbus API defined:
    - https://gitlab-master.nvidia.com/dgx/bmc/phosphor-dbus-interfaces/-/blob/develop/yaml/xyz/openbmc_project/State/FeatureReady.interface.yaml

Use case for state management should belong one of the categories below.

For an example let's take a look at FeatureReady interface:

> FeatureReady.interface.yaml
```
description: >
    Implement to indicate the states of any feature in openbmc.
properties:
    - name: State
      type: enum[self.States]
      description: >
        The object known state.
    - name: FeatureType
      type: enum[self.FeatureTypes]
      description: >
        The object known feature type

enumerations:
    - name: States
      description: >
          Possible state type
      values:
        - name: Enabled
          description: >
              This Feature is enabled.
        - name: StandbyOffline
          description: >
              This Feature is enabled but awaits an external
              action to activate it.
        - name: Starting
          description: >
              This Feature is starting.
        - name: Disabled
          description: >
              This Feature is disabled.
    - name: FeatureTypes
      description: >
          Possible feature type
      values:
        - name: Telemetry
          description: >
              This resource indicates about telemetry status
        - name: FWUpdate
          description: >
              This resource indicates about FW Update status
        - name: MC
          description: >
              This resource indicates about Management Controller status
```

### Preset Dbus Tree for CSM service
```
root@hgx:~# busctl tree xyz.openbmc_project.State.ConfigurableStateManager
`-/xyz
  `-/xyz/openbmc_project
    `-/xyz/openbmc_project/state
      `-/xyz/openbmc_project/state/configurableStateManager
        |-/xyz/openbmc_project/state/configurableStateManager/ChassisPower
        `-/xyz/openbmc_project/state/configurableStateManager/Telemetry
```

Below is the object corresponding to ChassisPower.json .The use case here is about reporting chassis power status. The property "CurrentPowerState" is the property to look for its status. The interface implemented here is  "xyz.openbmc_project.State.Chassis" . This use case comes under Power Category.

```
root@hgx:~# busctl introspect xyz.openbmc_project.State.ConfigurableStateManager /xyz/openbmc_project/state/configurableStateManager/ChassisPower
NAME                                TYPE      SIGNATURE RESULT/VALUE                             FLAGS
org.freedesktop.DBus.Introspectable interface -         -                                        -
.Introspect                         method    -         s                                        -
org.freedesktop.DBus.Peer           interface -         -                                        -
.GetMachineId                       method    -         s                                        -
.Ping                               method    -         -                                        -
org.freedesktop.DBus.Properties     interface -         -                                        -
.Get                                method    ss        v                                        -
.GetAll                             method    s         a{sv}                                    -
.Set                                method    ssv       -                                        -
.PropertiesChanged                  signal    sa{sv}as  -                                        -
xyz.openbmc_project.State.Chassis   interface -         -                                        -
.CurrentPowerState                  property  s         "xyz.openbmc_project.State.Chassis.Po... emits-change writable
.CurrentPowerStatus                 property  s         "xyz.openbmc_project.State.Chassis.Po... emits-change writable
.LastStateChangeTime                property  t         0                                        emits-change writable
.RequestedPowerTransition           property  s         "xyz.openbmc_project.State.Chassis.Tr... emits-change writable
```

Below is the object corresponding to TelemetryReady.json .The use case here is about reporting Telemetry Readiness status. The property "State" is the property to look for its status. The property "TypeInCategory" will have value Telemetry. The interface implemented here is "xyz.openbmc_project.State.FeatureReady" . This use case comes under FeatureReady Category.

```
root@hgx:~# busctl introspect xyz.openbmc_project.State.ConfigurableStateManager /xyz/openbmc_project/state/configurableStateManager/Telemetry
NAME                                   TYPE      SIGNATURE RESULT/VALUE                             FLAGS
org.freedesktop.DBus.Introspectable    interface -         -                                        -
.Introspect                            method    -         s                                        -
org.freedesktop.DBus.Peer              interface -         -                                        -
.GetMachineId                          method    -         s                                        -
.Ping                                  method    -         -                                        -
org.freedesktop.DBus.Properties        interface -         -                                        -
.Get                                   method    ss        v                                        -
.GetAll                                method    s         a{sv}                                    -
.Set                                   method    ssv       -                                        -
.PropertiesChanged                     signal    sa{sv}as  -                                        -
xyz.openbmc_project.State.FeatureReady interface -         -                                        -
.TypeInCategory                        property  s         "xyz.openbmc_project.State.FeatureRea... emits-change writable
.State                                 property  s         "xyz.openbmc_project.State.FeatureRea... emits-change writable
```

## Understanding JSON configuration File Structure 

All the json files are present under path **"/usr/share/configurable-state-manager"** on setup. Lets take TelemetryReady.json as an example.

> TelemetryReady.json
```
{
    "InterfaceName" : "xyz.openbmc_project.State.FeatureReady",
    "TypeInCategory": "xyz.openbmc_project.State.FeatureReady.FeatureTypes.Telemetry",
    "ServicesToBeMonitored": {
        "xyz.openbmc_project.State.Chassis": ["/xyz/openbmc_project/state/configurableStateManager/ChassisPower"],
        "xyz.openbmc_project.State.ServiceReady": ["/xyz/openbmc_project/GpuMgr", "/xyz/openbmc_project/inventory/metrics/platformmetrics"]
    },
    "State": {
        "State_property": "State",
        "Default": "xyz.openbmc_project.State.FeatureReady.States.StandbyOffline",
        "ConditionsFallback": "xyz.openbmc_project.State.FeatureReady.States.Starting",
        "States": {
            "xyz.openbmc_project.State.FeatureReady.States.StandbyOffline": {
                "Conditions": {
                    "xyz.openbmc_project.State.Chassis": {
                        "Property" : "CurrentPowerState",
                        "Value" : "xyz.openbmc_project.State.Chassis.PowerState.Off"
                    }
                }
            },
            "xyz.openbmc_project.State.FeatureReady.States.Enabled": {
                "Conditions": {
                    "xyz.openbmc_project.State.Chassis": {
                        "Property": "CurrentPowerState",
                        "Value": "xyz.openbmc_project.State.Chassis.PowerState.On"
                    },
                    "xyz.openbmc_project.State.ServiceReady": {
                        "Property" : "State",
                        "Value" : "xyz.openbmc_project.State.ServiceReady.States.Enabled",
                        "Logic": "AND"
                    }
                },
                "Logic": "AND"
            },
            "xyz.openbmc_project.State.FeatureReady.States.Starting": {
                "Conditions": {
                    "xyz.openbmc_project.State.Chassis": {
                        "Property": "CurrentPowerState",
                        "Value": "xyz.openbmc_project.State.Chassis.PowerState.On"
                    },
                    "xyz.openbmc_project.State.ServiceReady": {
                        "Property" : "State",
                        "Value" : "xyz.openbmc_project.State.ServiceReady.States.Starting",
                        "Logic": "OR"
                    }
                },
                "Logic": "AND"
            }
        }
    }
}
```

**InterfaceName -** this key will pass the interface name which we want to implement. It will be one of the category which we will pick for our use case as discussed in section - **Categories Defined for state management**. For TelemetryReadiness use-case we choose FeatureReady as category. This is the necessary field .
> **ex:** "InterfaceName" : "xyz.openbmc_project.State.FeatureReady"

**TypeInCategory -** this key will pass the type value for Properties like FeatureType/DeviceType/ServiceType etc defined in the interfaces. Like for TelemetryReadiness use case we are implementing dbus api **"xyz.openbmc_project.State.FeatureReady"**. TypeInCategory will pass the value for the property **"FeatureType"** in the interface above. TypeInCategory value should be from the enumeration defined. For above interface enumeration is **{Telemetry, FWUpdate, MC}**. This is the necessary field. If value intended is not present in enumeration, need to make change in PDI. This field will also provide name for sbus object. We will Telemetry substring from this string and create dbus object **"/xyz/openbmc_project/state/configurableStateManager/Telemetry"**
> **ex:** "TypeInCategory": "xyz.openbmc_project.State.FeatureReady.FeatureTypes.Telemetry"

**ServicesToBeMonitored -** this key will pass a map of { interface-name : [array of object-paths implementing the particular interface] } we want to monitor. These combinations will be responsible in transition of states for the particular use case. This is the necessary field.
> **ex:** 
```
"ServicesToBeMonitored": {
        "xyz.openbmc_project.State.Chassis": ["/xyz/openbmc_project/state/configurableStateManager/ChassisPower"],
        "xyz.openbmc_project.State.ServiceReady": ["/xyz/openbmc_project/GpuMgr", "/xyz/openbmc_project/inventory/metrics/platformmetrics"]
    }
```
**State -** this key will contain the whole transition logic for the use case 
> **ex:** "State": { //transition logic }

**State_property -** This key will pass the name of property which will report the state for the current use case. Like for TelemetryReadiness we have interface **"xyz.openbmc_project.State.FeatureReady"**. It has property name **"State"**. For **"xyz.openbmc_project.State.Chassis"** it is **"CurentPowerState"**. 
> **ex:** "State_property": "State"

**Default -** this key will pass the value of state that will be set at the time startup of the service for that interface. The property name in "State_property" field will be set to this default value. So for use case in interface - **"xyz.openbmc_project.State.FeatureReady"** the **State" property will be set to this default value. Default value will be one of the values in enumerations defined in PDI for "State" property. 
> **ex:** "Default": "xyz.openbmc_project.State.FeatureReady.States.StandbyOffline"

**ConditionsFallback -** this key will pass the value when no conditons evaluates to true in transition conditions. Its kind of worst case handling.
> **ex:** "ConditionsFallback": "xyz.openbmc_project.State.FeatureReady.States.Starting"

**States -**   It is a map of {value of "State_Property : conditions to be met} . Basically it is the enumerations on all values with its conditions.
Whenever conditions are true that particular value is set.
> **ex:** 
```
"States": {
            "xyz.openbmc_project.State.FeatureReady.States.StandbyOffline": {
                "Conditions": {
                    "xyz.openbmc_project.State.Chassis": {
                        "Property" : "CurrentPowerState",
                        "Value" : "xyz.openbmc_project.State.Chassis.PowerState.Off"
                    }
                }
            },
            "xyz.openbmc_project.State.FeatureReady.States.Enabled": {
                "Conditions": {
                    "xyz.openbmc_project.State.Chassis": {
                        "Property": "CurrentPowerState",
                        "Value": "xyz.openbmc_project.State.Chassis.PowerState.On"
                    },
                    "xyz.openbmc_project.State.ServiceReady": {
                        "Property" : "State",
                        "Value" : "xyz.openbmc_project.State.ServiceReady.States.Enabled",
                        "Logic": "AND"
                    }
                },
                "Logic": "AND"
            }
        }
```

**Conditions -** Each entry of Conditions block will have interface to be monitored as a key, property name and value to watch under that interface. We already have **ServicesToBeMonitored** block. From that we will get all services implementing that interface. Now we will have combination of ** <interface, objectpath, PropertyName, PropertyValue> **. Now for each type of combination we will check whether PropertyName has value equal to PropertyValue. This is a necessary field.

**Important** - If there is a single combination present there is no need of "Logical" field. So logical field is optional. If there is a single combination present means it is kind of "==" operation. 

Now if there are more than 1 combination present then we need "Logical" field. Supported logical fields are "AND" and "OR". 

Lets understand the conditons block below.

```
"xyz.openbmc_project.State.FeatureReady.States.Enabled": {
                "Conditions": {
                    "xyz.openbmc_project.State.Chassis": {
                        "Property": "CurrentPowerState",
                        "Value": "xyz.openbmc_project.State.Chassis.PowerState.On"
                    },
                    "xyz.openbmc_project.State.ServiceReady": {
                        "Property" : "State",
                        "Value" : "xyz.openbmc_project.State.ServiceReady.States.Enabled",
                        "Logic": "AND"           **//individual entry logic**
                    }
                },
                "Logic": "AND"                 **//parent logic**
            }
```

**Entry1** 
```
 "xyz.openbmc_project.State.Chassis": {
                        "Property": "CurrentPowerState",
                        "Value": "xyz.openbmc_project.State.Chassis.PowerState.On"
                    }
```

Entry_1_Combination_1 
- interface - "xyz.openbmc_project.State.Chassis"
- Property - "CurrentPowerState"
- Value - "xyz.openbmc_project.State.Chassis.PowerState.On"
- Object - "/xyz/openbmc_project/state/configurableStateManager/ChassisPower"

As there is single combination no need of Logical field.

**Entry2** 
```
"xyz.openbmc_project.State.ServiceReady": {
                        "Property" : "State",
                        "Value" : "xyz.openbmc_project.State.ServiceReady.States.Enabled",
                        "Logic": "AND"
                    }
```

Here combinations are-

For interface xyz.openbmc_project.State.ServiceReady we get 2 object paths "/xyz/openbmc_project/GpuMgr", "/xyz/openbmc_project/inventory/metrics/platformmetrics"

Entry_2_Combination_1
- interface - "xyz.openbmc_project.State.ServiceReady"
- Property - "State"
- Value - "xyz.openbmc_project.State.ServiceReady.States.Enabled"
- Object - "/xyz/openbmc_project/GpuMgr"

Entry_2_Combination_2
- interface - "xyz.openbmc_project.State.ServiceReady"
- Property - "State"
- Value - "xyz.openbmc_project.State.ServiceReady.States.Enabled"
- Object - "/xyz/openbmc_project/inventory/metrics/platformmetrics"

For entry2 -> we have "Logic": "AND" means "Entry_2_Combination_1 AND Entry_2_Combination_2"

Now For condition key "xyz.openbmc_project.State.FeatureReady.States.Enabled" parent logic is "AND". Hence complete logic is
> Entry_1_Combination_1 **AND** ( Entry_2_Combination_1 **AND** Entry_2_Combination_2 )

> This upper logic says **"State"** value will be set to **"xyz.openbmc_project.State.FeatureReady.States.Enabled"** when chassis power is ON (entry1) and all subservices gpuMgr and sensorServer are Enabled(Entry2).


On similar lines we have ChassisPower.json
> ChassisPower.json

```
{
    "InterfaceName":"xyz.openbmc_project.State.Chassis",
    "TypeInCategory": "ChassisPower",
    "ServicesToBeMonitored":{
        "xyz.openbmc_project.GpioStatus": ["/xyz/openbmc_project/GpioStatusHandler"]
    },
    "State":{
        "State_property": "CurrentPowerState",
        "Default": "xyz.openbmc_project.State.Chassis.PowerState.Off",
        "ConditionsFallback": "xyz.openbmc_project.State.Chassis.PowerState.On",
        "States":{
            "xyz.openbmc_project.State.Chassis.PowerState.On": {
                "Conditions" : {
                    "xyz.openbmc_project.GpioStatus": {
                        "Property" : "GPU_BASE_PWR_GD",
                        "Value": "true"
                    }
                }
            },
            "xyz.openbmc_project.State.Chassis.PowerState.Off": {
                "Conditions" : {
                    "xyz.openbmc_project.GpioStatus": {
                        "Property" : "GPU_BASE_PWR_GD",
                        "Value": "false"
                    }
                }
            }
        }
    }
}
```

## Flowchart of CSM
```
     +---------------------+
     |      Start          |
     +-------─┬─-----------+
              │ 
              ▼ 
     +------------------------------+
     | Initialize D-Bus Connection  |
     +------------------------------+
              |
              v
     +---------------------+
     | Create ObjectManager|
     +---------------------+
              |
              v
     +------------------------------+
     | Define JSON Config File Path |
     +------------------------------+
              |
              v
     +----------------------------------+
     | List JSON Files in Directory     |
     | and Sort filenames alphabetically|  
     +----------------------------------+
              |
              v
     +--------------------------+
     | Loop Over Each JSON File |   
     +--------------------------+
              |                                                                                                     +------------------------+
              v                                                                                                     |                        |
     +---------------------+                                                                   ^----->------------> |  executeTransition()   |
     | Parse JSON File     |                                                                   |                    |                        |
     +---------------------+                                                                   |                    +------------------------+
              |                                                                                |                            |
              v                                                                                ^                            v
     +-------------------------------+                   +---------------------+               |            +-------------------------------------------+
     | Check for JSON Parsing Errors |------------------>| throw runtime error |               |            | - Loop over all the state values that can |
     +-------------------------------+                   +---------------------+               |            |   be attained and evaluate corresponding  |-----<------
              |                                                                                |            |   conditions.                             |           | jump to
              v                                                                                ^            +-------------------------------------------+           | next state
     +------------------------------------------+                                              |                     |                        |                     | evaluation
     | Extract Interface Name, Feature Type etc.|                                              |                     v (true)                 v (false)             ^
     +------------------------------------------+                                              |                     |                        |                     |
              |                                                                                |        +-------------------------+    +----------------------+     |
              v                                                                                |        | - set the state value   |    | - log error message  |------   
     +-----------------------------------------------------------------------------------      |        | - return                |    |                      |
     | Create State Machine Entities                                                     |     |        +-------------------------+    +----------------------+
     | - on object creation set default value and type property                          |     |                                               | 
     | - execute transition() for init  -------------------------------------------->----------^                                               | if all
     | - create matchPtr1 with interfacesAdded rule for servicesToBeMonitored block      |     |                                               v state valuation
     |   because some services may not have start at the time of object creation of      |     |                                               | fails
     |   use case. whenever interfaceAdded on the path --------> executeTransition().----->----^                                               |
     | - create matchPtr2 for PropertiesChanged rule for servicesToBeMonitored block     |     |                                  +----------------------------------------------------------+
     |   becuase present state machine entity state depends on some properties in        |     |                                  |- log error message, set ConditionFallbacks value to state|
     |   the interface on object path in servicesToBeMonitored block. Whenever any       |     |                                  |- return                                                  |
     |   interested property changes -------> executeTransiton()----------------------->-------^                                  +----------------------------------------------------------+
     | - move all matchPtrs Respective vectors                                           |
     +-----------------------------------------------------------------------------------+
              |
              v
     +-------------------------------------+
     | Add Entities to Respective Vectors  |
     +-------------------------------------+
              |
              v
     +------------------------+
     | Start Asio I/O Service |
     +------------------------+
 ```

## End to End flow of CSM service block diagram for Telemetry Ready use case
```
                    ┌──────────────────┐
                    │    Redfish       │
                    └──────┬───────────┘
    Async DBus Calls       │   ▲
                           ▼   │
                    ┌──────────┴───────┐
                    │      D-Bus       │
                    └──────┬───────────┘
       D-Bus req/Res       │  ▲
                           ▼  │
         ┌────────────────────┴─────────────────────────────────────────────────────────────┐
         │                        Configurable State Manager                                │
         │ ┌──────────────┐                                          ┌───────────────┐      │
         │ │ ChassisPower │____________________<<____________________│ TelemetryReady|      │
         │ |              |____________________>>____________________|               |      |
         | └──────────────┘   (PropertyChange and InterfaceAdded     └───────────────┘      │
         │                     dbus signals)                                                │
         └──────────────────────────────────────────────────────────────────────────────────┘
(Property  │  ▲          │  ▲ (InterfaceAdded           (Property    │  ▲             │  ▲ (InterfaceAdded
change dbus▼  │          ▼  │  dbus signal)             change dbus  ▼  │             ▼  │  dbus signal)
signal)  ┌────┴──────────────┐                          signal)  ┌────┴─────────────────┐    
         │       GpuMgr      │                                   |   Sensor server      |
         └───────────────────┘                                   └──────────────────────┘
```

### Special scenarios Handled
- If any json file is corrupt it will throw runtime error.
- To handle dependency between different object paths in csm, we first sort the json filenames and then start parsing the json file. Like in the TelemetryReadiness use case Telmetry object has dependency chassisPower object. This is handled because chassisPower object has file name ChassisPower.json which will be processed before telemtry object which has file name Telemetry.json.
- If any error comes in fetching values for a particular json, it logs error and move to processing of other json.
- In executeTransition() we loop over state values with its conditions, if we get error while evaluating one state value, will log error for it and move to next state evaluation.

## Testing
 Test plan - https://docs.google.com/spreadsheets/d/1mVvJW98C-LO0vZYEE9vWfWfYyTCsL7ObCmlNrcpOJts/edit?userstoinvite=spasha@nvidia.com&sharingaction=manageaccess&role=writer#gid=0