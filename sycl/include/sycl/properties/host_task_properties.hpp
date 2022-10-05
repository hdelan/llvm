//==----------- buffer_properties.hpp --- SYCL buffer properties -----------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include <sycl/context.hpp>
#include <sycl/detail/property_helper.hpp>
#include <sycl/properties/property_traits.hpp>

namespace sycl {
__SYCL_INLINE_VER_NAMESPACE(_V1) {

namespace property {
namespace host_task {
class exec_on_submit
    : public detail::DataLessProperty<detail::HostTaskExecOnSubmit> {};

class manual_interop_sync
    : public detail::DataLessProperty<detail::HostTaskManualInteropSync> {};

} // namespace host_task
} // namespace property

// Forward declaration
class host_task;

template <>
struct is_property<property::host_task::exec_on_submit> : std::true_type {};
template <>
struct is_property<property::host_task::manual_interop_sync> : std::true_type {
};

template <>
struct is_property_of<property::host_task::exec_on_submit, host_task>
    : std::true_type {};
template <>
struct is_property_of<property::host_task::manual_interop_sync, host_task>
    : std::true_type {};

} // __SYCL_INLINE_VER_NAMESPACE(_V1)
} // namespace sycl
