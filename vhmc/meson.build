# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
# AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Built only when vhmc-state-manager is enabled; see the guard in the root meson.
executable(
    'phosphor-vhmc-state-manager',
    'vhmc_state_manager.cpp',
    'vhmc_state_manager_main.cpp',
    dependencies: [phosphordbusinterfaces, phosphorlogging, sdbusplus],
    include_directories: include_directories('..'),
    implicit_include_directories: true,
    install: true,
)
