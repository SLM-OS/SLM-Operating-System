/**
 * Copyright (c) 2019-2026 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
/**
 * @file hef.cpp
 * @brief TODO: brief
 *
 * TODO: doc
 **/

#include "hailo/hailort.h"
#include "hailo/hef.hpp"
#include "hailo/stream.hpp"
#include "hailo/device.hpp"
#include "hailo/hailort_common.hpp"
#include "hailo/hailort_defaults.hpp"
#include "hailo/quantization.hpp"

#include "common/utils.hpp"
#include "common/logger_macros.hpp"
#include "common/genai/constants.hpp"
#include "common/genai/serializer/serializer.hpp"

#include "net_flow/ops/nms_post_process.hpp"
#include "net_flow/ops/yolov5_post_process.hpp"
#include "net_flow/ops/yolov5_bbox_only_post_process.hpp"
#include "net_flow/ops/yolox_post_process.hpp"
#include "net_flow/ops/ssd_post_process.hpp"
#include "net_flow/ops/argmax_post_process.hpp"
#include "net_flow/ops/softmax_post_process.hpp"
#include "net_flow/ops/yolov5_seg_post_process.hpp"
#include "net_flow/ops/yolov8_post_process.hpp"
#include "net_flow/ops/yolov8_bbox_only_post_process.hpp"
#include "hef/hef_internal.hpp"
#include "vdma/legacy_pcie/legacy_pcie_device.hpp"
#include "vdma/vdma_config_manager.hpp"
#include "hef/layer_info.hpp"
#include "device_common/control.hpp"
#include "utils/profiler/tracer_macros.hpp"

#include "byte_order.h"
#include "context_switch_defs.h"

#include <fstream>
#include <memory>
#include <limits>
#include <stdint.h>
#include <stdbool.h>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <numeric>
#include <google/protobuf/io/zero_copy_stream_impl.h>


namespace hailort
{

#define HEF__MD5_BUFFER_SIZE (1024)
#define DEFAULT_BATCH_SIZE (1)
#define SKIP_SPACE_COMMA_CHARACTERS (2)
#define ALIGNED_TO_4_BYTES (4)
#define MIN_SLEEP_TIME_USEC (1000)
constexpr uint8_t DEFAULT_DIVISION_FACTOR = 1;

static const std::string MISSING_SDK_VERSION = "0.0.0";
#define TAB ("    ")

static std::string add_tabs(uint8_t count)
{
    // Each TAB counts as 4 spaces
    std::string res = "";
    for (uint8_t i = 0; i < count; i++) {
        res = res + TAB;
    }
    return res;
}

static std::string get_shape_str(const hailo_stream_info_t &stream_info)
{
    switch (stream_info.format.order)
    {
    case HAILO_FORMAT_ORDER_HAILO_NMS_ON_CHIP:
        return HailoRTCommon::get_format_type_str(stream_info.format.type) + ", " + HailoRTCommon::get_format_order_str(stream_info.format.order) +
            "(maximum frame size: " + std::to_string(HailoRTCommon::get_nms_hw_frame_size(stream_info.nms_info)) + ")";

    case HAILO_FORMAT_ORDER_NC:
        return HailoRTCommon::get_format_type_str(stream_info.format.type) + ", " + HailoRTCommon::get_format_order_str(stream_info.format.order) +
            "(" + std::to_string(stream_info.hw_shape.features) + ")";
    case HAILO_FORMAT_ORDER_NHW:
        return HailoRTCommon::get_format_type_str(stream_info.format.type) + ", " + HailoRTCommon::get_format_order_str(stream_info.format.order) +
            "(" + std::to_string(stream_info.hw_shape.height) + "x" + std::to_string(stream_info.hw_shape.width) + ")";
    default:
        return HailoRTCommon::get_format_type_str(stream_info.format.type) + ", " + HailoRTCommon::get_format_order_str(stream_info.format.order) +
            "(" + std::to_string(stream_info.hw_shape.height) + "x" + std::to_string(stream_info.hw_shape.width) +
            "x" + std::to_string(stream_info.hw_shape.features) + ")";
    }
}

static std::string get_shape_str(const hailo_vstream_info_t &vstream_info)
{
    switch (vstream_info.format.order)
    {
    case HAILO_FORMAT_ORDER_HAILO_NMS_BY_CLASS:
        return HailoRTCommon::get_format_type_str(vstream_info.format.type) + ", " + HailoRTCommon::get_format_order_str(vstream_info.format.order) +
            "(number of classes: " + std::to_string(vstream_info.nms_shape.number_of_classes) +
            ", maximum bounding boxes per class: " + std::to_string(vstream_info.nms_shape.max_bboxes_per_class) +
            ", maximum frame size: " + std::to_string(HailoRTCommon::get_nms_host_frame_size(vstream_info.nms_shape, vstream_info.format)) + ")";
    case HAILO_FORMAT_ORDER_HAILO_NMS_BY_SCORE:
    case HAILO_FORMAT_ORDER_HAILO_NMS_WITH_BYTE_MASK:
        return HailoRTCommon::get_format_type_str(vstream_info.format.type) + ", " + HailoRTCommon::get_format_order_str(vstream_info.format.order) +
            "(number of classes: " + std::to_string(vstream_info.nms_shape.number_of_classes) +
            ", maximum bounding boxes total: " + std::to_string(vstream_info.nms_shape.max_bboxes_total) +
            ", maximum frame size: " + std::to_string(HailoRTCommon::get_nms_host_frame_size(vstream_info.nms_shape, vstream_info.format)) + ")";
    case HAILO_FORMAT_ORDER_NC:
        return HailoRTCommon::get_format_type_str(vstream_info.format.type) + ", " + HailoRTCommon::get_format_order_str(vstream_info.format.order) +
            "(" + std::to_string(vstream_info.shape.features) + ")";
    case HAILO_FORMAT_ORDER_NHW:
        return HailoRTCommon::get_format_type_str(vstream_info.format.type) + ", " + HailoRTCommon::get_format_order_str(vstream_info.format.order) +
            "(" +std::to_string(vstream_info.shape.height) + "x" + std::to_string(vstream_info.shape.width) + ")";
    default:
        return HailoRTCommon::get_format_type_str(vstream_info.format.type) + ", " + HailoRTCommon::get_format_order_str(vstream_info.format.order) +
            "(" + std::to_string(vstream_info.shape.height) + "x" + std::to_string(vstream_info.shape.width) + "x" +
            std::to_string(vstream_info.shape.features) + ")";
    }
}

bool ConfigureNetworkParams::operator==(const ConfigureNetworkParams &other) const
{
    for (auto &name_param_pair : network_params_by_name) {
        if ((other.network_params_by_name.find(name_param_pair.first) == other.network_params_by_name.end()) ||
                (name_param_pair.second.batch_size != other.network_params_by_name.at(name_param_pair.first).batch_size) ) {
            return false;
        }
    }
    return (batch_size == other.batch_size) && (power_mode == other.power_mode) && (latency == other.latency);
}

bool ConfigureNetworkParams::operator!=(const ConfigureNetworkParams &other) const
{
    return !(*this == other);
}

// Note: Can't add the definition in the header. This will lead to the following error:
//       /usr/include/c++/7/bits/unique_ptr.h: In instantiation of 'void std::default_delete<_Tp>::operator()(_Tp*) const [with _Tp = Hef::Impl]':
//       /usr/include/c++/7/bits/unique_ptr.h:263:17:   required from 'std::unique_ptr<_Tp, _Dp>::~unique_ptr() [with _Tp = Hef::Impl; _Dp = std::default_delete<Hef::Impl>]'
//       /local/users/projects/platform-sw/hailort/libhailort/src/../include/hailo/hef.hpp:61:7:   required from 'Expected<T>::~Expected() [with T = Hef]'
//       /local/users/projects/platform-sw/hailort/hailortcli/run_command.cpp:705:51:   required from here
//       /usr/include/c++/7/bits/unique_ptr.h:76:22: error: invalid application of 'sizeof' to incomplete type 'Hef::Impl'
//         static_assert(sizeof(_Tp)>0,
Hef::~Hef() = default;
Hef::Hef(Hef &&) = default;
Hef &Hef::operator=(Hef &&) = default;

Expected<Hef> Hef::create(const std::string &hef_path)
{
    TRY(auto impl, Hef::Impl::create(hef_path));
    auto impl_ptr = make_shared_nothrow<Impl>(std::move(impl));
    CHECK_NOT_NULL_AS_EXPECTED(impl_ptr, HAILO_OUT_OF_HOST_MEMORY);
    return Hef(std::move(impl_ptr));
}

Expected<Hef> Hef::create(const MemoryView &hef_buffer)
{
    TRY(auto hef_shared_buffer, Buffer::create_shared(hef_buffer.data(), hef_buffer.size(),
        BufferStorageParams::create_dma()));
    TRY(auto impl, Hef::Impl::create(hef_shared_buffer));
    auto impl_ptr = make_shared_nothrow<Impl>(std::move(impl));
    CHECK_NOT_NULL_AS_EXPECTED(impl_ptr, HAILO_OUT_OF_HOST_MEMORY);
    return Hef(std::move(impl_ptr));
}

Expected<Hef> Hef::create(std::shared_ptr<Buffer> hef_buffer)
{
    TRY(auto impl, Hef::Impl::create(hef_buffer));
    auto impl_ptr = make_shared_nothrow<Impl>(std::move(impl));
    CHECK_NOT_NULL_AS_EXPECTED(impl_ptr, HAILO_OUT_OF_HOST_MEMORY);
    return Hef(std::move(impl_ptr));
}

Hef::Hef(std::shared_ptr<Impl> pimpl) :
    pimpl(std::move(pimpl))
{}

Expected<std::vector<hailo_stream_info_t>> Hef::get_input_stream_infos(const std::string &name) const
{
    TRY(const auto network_pair, pimpl->get_network_group_and_network_name(name));
    return pimpl->get_input_stream_infos(network_pair.first, network_pair.second);
}

Expected<std::vector<hailo_stream_info_t>> Hef::get_output_stream_infos(const std::string &name) const
{
    TRY(const auto network_pair, pimpl->get_network_group_and_network_name(name));
    return pimpl->get_output_stream_infos(network_pair.first, network_pair.second);
}

Expected<std::vector<hailo_stream_info_t>> Hef::get_all_stream_infos(const std::string &name) const
{
    TRY(const auto network_pair, pimpl->get_network_group_and_network_name(name));
    return pimpl->get_all_stream_infos(network_pair.first, network_pair.second);
}

Expected<std::vector<hailo_network_info_t>> Hef::get_network_infos(const std::string &net_group_name) const
{
    TRY(const auto names_pair, pimpl->get_network_group_and_network_name(net_group_name));
    return pimpl->get_network_infos(names_pair.first);
}

Expected<hailo_stream_info_t> Hef::get_stream_info_by_name(const std::string &stream_name,
    hailo_stream_direction_t stream_direction, const std::string &net_group_name) const
{
    // Addressing the situation where net_group_name == ""
    TRY(const auto net_group_name_pair, pimpl->get_network_group_and_network_name(net_group_name));
    const auto &net_group_name_str = net_group_name_pair.first;

    return pimpl->get_stream_info_by_name(stream_name, stream_direction, net_group_name_str);
}

Expected<std::vector<hailo_vstream_info_t>> Hef::get_input_vstream_infos(const std::string &name) const
{
    TRY(const auto network_pair, pimpl->get_network_group_and_network_name(name));
    return pimpl->get_input_vstream_infos(network_pair.first, network_pair.second);
}

Expected<std::vector<hailo_vstream_info_t>> Hef::get_output_vstream_infos(const std::string &name) const
{
    TRY(const auto network_pair, pimpl->get_network_group_and_network_name(name));
    return pimpl->get_output_vstream_infos(network_pair.first, network_pair.second);
}

Expected<std::vector<hailo_vstream_info_t>> Hef::get_all_vstream_infos(const std::string &name) const
{
    TRY(const auto network_pair, pimpl->get_network_group_and_network_name(name));
    return pimpl->get_all_vstream_infos(network_pair.first, network_pair.second);
}

Expected<std::vector<std::string>> Hef::get_sorted_output_names(const std::string &net_group_name) const
{
    // Addressing the situation where net_group_name == ""
    TRY(const auto net_group_name_pair, pimpl->get_network_group_and_network_name(net_group_name));
    const auto &net_group_name_str = net_group_name_pair.first;
    return pimpl->get_sorted_output_names(net_group_name_str);
}

Expected<size_t> Hef::get_number_of_input_streams(const std::string &net_group_name) const
{
    // Addressing the situation where net_group_name == ""
    TRY(const auto net_group_name_pair, pimpl->get_network_group_and_network_name(net_group_name));
    const auto &net_group_name_str = net_group_name_pair.first;
    return pimpl->get_number_of_input_streams(net_group_name_str);
}

Expected<size_t> Hef::get_number_of_output_streams(const std::string &net_group_name) const
{
    // Addressing the situation where net_group_name == ""
    TRY(const auto net_group_name_pair, pimpl->get_network_group_and_network_name(net_group_name));
    const auto &net_group_name_str = net_group_name_pair.first;
    return pimpl->get_number_of_output_streams(net_group_name_str);
}

Expected<float64_t> Hef::get_bottleneck_fps(const std::string &net_group_name) const
{
    return pimpl->get_bottleneck_fps(net_group_name);
}


Expected<hailo_device_architecture_t> Hef::get_hef_device_arch() const
{
    TRY(auto compatible_archs, get_compatible_device_archs());
    return Expected<hailo_device_architecture_t>{compatible_archs.at(0)};
}

Expected<std::vector<hailo_device_architecture_t>> Hef::get_compatible_device_archs() const
{
    return DeviceBase::hef_arch_to_device_compatible_archs(static_cast<HEFHwArch>(pimpl->get_device_arch()));
}

Expected<std::string> Hef::device_arch_to_string(const hailo_device_architecture_t arch)
{
    return HailoRTCommon::get_device_arch_str(arch);
}

Expected<std::string> Hef::get_vstream_name_from_original_name(const std::string &original_name,
    const std::string &net_group_name) const
{
    return pimpl->get_vstream_name_from_original_name(original_name, net_group_name);
}

Expected<std::vector<std::string>> Hef::get_original_names_from_vstream_name(const std::string &stream_name,
    const std::string &net_group_name) const
{
    return pimpl->get_original_names_from_vstream_name(stream_name, net_group_name);
}

Expected<std::vector<std::string>> Hef::get_stream_names_from_vstream_name(const std::string &vstream_name,
    const std::string &net_group_name) const
{
    TRY(const auto network_group_name_pair, pimpl->get_network_group_and_network_name(net_group_name));
    const auto &net_group_name_str = network_group_name_pair.first;
    return pimpl->get_stream_names_from_vstream_name(vstream_name, net_group_name_str);
}

Expected<std::vector<std::string>> Hef::get_vstream_names_from_stream_name(const std::string &stream_name,
    const std::string &net_group_name) const
{
    TRY(const auto network_group_name_pair, pimpl->get_network_group_and_network_name(net_group_name));
    const auto &net_group_name_str = network_group_name_pair.first;
    return pimpl->get_vstream_names_from_stream_name(stream_name, net_group_name_str);
}

Expected<Hef::Impl> Hef::Impl::create(const std::string &hef_path)
{
    hailo_status status = HAILO_UNINITIALIZED;

    Impl hef(hef_path, status);
    if (HAILO_SUCCESS != status) {
        LOGGER__ERROR("Failed creating HEF");
        return make_unexpected(status);
    }
    return hef;
}

Expected<Hef::Impl> Hef::Impl::create(std::shared_ptr<Buffer> hef_buffer)
{
    hailo_status status = HAILO_UNINITIALIZED;

    Impl hef(hef_buffer, status);
    if (HAILO_SUCCESS != status) {
        LOGGER__ERROR("Failed creating HEF");
        return make_unexpected(status);
    }

    return hef;
}

Expected<size_t> calc_hef_residue_size(std::shared_ptr<SeekableBytesReader> hef_reader, uint32_t version)
{
    TRY(auto total_size, hef_reader->get_size());
    switch (version) {
    case HEADER_VERSION_0:
        return total_size - HEF_HEADER_SIZE_V0;
    case HEADER_VERSION_1:
        return total_size - HEF_HEADER_SIZE_V1;
    case HEADER_VERSION_2:
        return total_size - HEF_HEADER_SIZE_V2;
    case HEADER_VERSION_3:
        return total_size - HEF_HEADER_SIZE_V3;
    default:
        LOGGER__ERROR("Unsupported hef version {}", version);
        return make_unexpected(HAILO_HEF_NOT_SUPPORTED);
    }
}

static hailo_status calc_buffer_md5(const uint8_t *buffer, const size_t buffer_size, MD5_SUM_t &calculated_md5)
{
    MD5_CTX md5 = {};
    MD5_Init(&md5);
    MD5_Update(&md5, buffer, buffer_size);
    MD5_Final(calculated_md5, &md5);

    return HAILO_SUCCESS;
}

static hailo_status calc_istream_md5(std::ifstream &s, MD5_SUM_t &calculated_md5)
{
    char md5_buffer[HEF__MD5_BUFFER_SIZE] = {};
    MD5_CTX md5 = {};
    auto beg_pos = s.tellg();
    CHECK(-1 != beg_pos, HAILO_FILE_OPERATION_FAILURE, "ifstream::tellg() failed");
    MD5_Init(&md5);
    while (!s.eof()) {
        s.read(md5_buffer, HEF__MD5_BUFFER_SIZE);
        CHECK(!s.bad(), HAILO_FILE_OPERATION_FAILURE, "ifstream::read() failed");
        MD5_Update(&md5, &md5_buffer, s.gcount());
    }
    MD5_Final(calculated_md5, &md5);
    s.clear();
    s.seekg(beg_pos, s.beg);
    CHECK(s.good(), HAILO_FILE_OPERATION_FAILURE, "ifstream::seekg() failed");
    return HAILO_SUCCESS;
}

hailo_status Hef::Impl::validate_hef_header(const hef__header_t &header, MD5_SUM_t &calculated_md5, size_t hef_file_residue_size)
{
    CHECK(HEADER_MAGIC == header.magic, HAILO_HEF_NOT_SUPPORTED,
        "HEF magic does not match. detected magic - {:x}", header.magic);

    CHECK((HEADER_VERSION_0 == header.version) , HAILO_INTERNAL_FAILURE,
        "HEF version does not match. Should be {} but detected {}", HEADER_VERSION_0, header.version);

    CHECK(hef_file_residue_size == header.hef_proto_size, HAILO_HEF_FILE_CORRUPTED,
        "HEF file length does not match");

    CHECK(0 == memcmp(&calculated_md5, &header.distinct.v0.expected_md5, sizeof(MD5_SUM_t)), HAILO_HEF_FILE_CORRUPTED,
        "HEF md5 does not match");

    return HAILO_SUCCESS;
}

hailo_status Hef::Impl::validate_hef_header(const hef__header_t &header, const uint32_t &crc_32, size_t hef_file_residue_size)
{
    CHECK(HEADER_MAGIC == header.magic, HAILO_HEF_NOT_SUPPORTED,
        "HEF magic does not match. Should be {:x} but detected magic - {:x}", HEADER_MAGIC, header.magic);

    CHECK((HEADER_VERSION_1 == header.version), HAILO_INTERNAL_FAILURE,
        "HEF version does not match. Should be {} but detected {}", HEADER_VERSION_1, header.version);

    CHECK(hef_file_residue_size == header.hef_proto_size + header.distinct.v1.ccws_size, HAILO_HEF_FILE_CORRUPTED,
        "HEF file length does not match");

    CHECK(0 == memcmp(&crc_32, &header.distinct.v1.crc, sizeof(crc_32)), HAILO_HEF_FILE_CORRUPTED,
        "HEF crc does not match");

    return HAILO_SUCCESS;
}

hailo_status Hef::Impl::validate_hef_header(const hef__header_t &header, const uint64_t &calculated_xxh3_64bits, size_t hef_file_residue_size)
{
    CHECK(HEADER_MAGIC == header.magic, HAILO_HEF_NOT_SUPPORTED,
        "HEF magic does not match. Should be {:x} but detected magic - {:x}", HEADER_MAGIC, header.magic);

    uint64_t non_proto_size = 0;
    uint64_t xxh3_64bits_from_hef = 0;
    if (HEADER_VERSION_2 == header.version) {
        non_proto_size = header.distinct.v2.ccws_size;
        xxh3_64bits_from_hef = header.distinct.v2.xxh3_64bits;
    } else if (HEADER_VERSION_3 == header.version) {
        non_proto_size = header.distinct.v3.ccws_size_with_padding + header.distinct.v3.additional_info_size;
        xxh3_64bits_from_hef = header.distinct.v3.xxh3_64bits;
    } else {
        LOGGER__ERROR("Invalid HEF version");
        return HAILO_HEF_NOT_SUPPORTED;
    }
    CHECK(hef_file_residue_size == header.hef_proto_size + non_proto_size, HAILO_HEF_FILE_CORRUPTED,
       "HEF file length does not match");

    CHECK(0 == memcmp(&calculated_xxh3_64bits, &xxh3_64bits_from_hef, sizeof(calculated_xxh3_64bits)), HAILO_HEF_FILE_CORRUPTED,
        "HEF xxhash does not match, calculated: {}, expected: {}", calculated_xxh3_64bits, xxh3_64bits_from_hef);

    return HAILO_SUCCESS;
}

hailo_status Hef::Impl::validate_hef_extensions()
{
    std::vector<std::string> unsupported_extensions;
    for (const auto &extension : m_hef_extensions) {
        if ((extension.type_index() >= m_supported_extensions_bitset.size()) || !m_supported_extensions_bitset.test(extension.type_index())) {
            unsupported_extensions.emplace_back(extension.name());
        }
    }

    CHECK(unsupported_extensions.empty(), HAILO_HEF_NOT_SUPPORTED, "Failed opening non-compatible HEF with the following unsupported extensions: {}",
        std::accumulate(std::next(unsupported_extensions.begin()), unsupported_extensions.end(), unsupported_extensions[0], 
        [] (std::string a, std::string b) { return std::move(a) + ", " + b; }));

    CHECK_AS_EXPECTED(m_supported_features.periph_calculation_in_hailort, HAILO_HEF_NOT_SUPPORTED,
        "Hef has periph_calculation_in_hailort feature disabled - this HEF is outdated and no longer supported. Please update HEF");

    return HAILO_SUCCESS;
}

void Hef::Impl::init_md5(MD5_SUM_t &calculated_md5)
{
    memcpy(m_md5, calculated_md5, sizeof(m_md5));
}

void Hef::Impl::init_crc(uint32_t crc_32)
{
    memcpy(&m_crc, &crc_32, sizeof(crc_32));
}

void Hef::Impl::init_hef_version(uint32_t version)
{
    m_hef_version = version;
}

Expected<hef__header_t> Hef::Impl::parse_hef_header_before_distinct(std::shared_ptr<SeekableBytesReader> hef_reader)
{
    hef__header_t hef_header = {};
    auto status = hef_reader->read(reinterpret_cast<uint8_t*>(&hef_header), HEF_COMMON_SIZE);
    CHECK_SUCCESS_AS_EXPECTED(status);

    hef_header.magic = BYTE_ORDER__htonl(hef_header.magic);
    hef_header.version = BYTE_ORDER__htonl(hef_header.version);
    hef_header.hef_proto_size = BYTE_ORDER__htonl(hef_header.hef_proto_size);

    return hef_header;
}

hailo_status Hef::Impl::fill_v1_hef_header(hef__header_t &hef_header, std::shared_ptr<SeekableBytesReader> hef_reader)
{
    auto status = hef_reader->read(reinterpret_cast<uint8_t*>(&hef_header.distinct), sizeof(hef__header_distinct_t::v1));
    CHECK_SUCCESS(status);

    hef_header.distinct.v1.ccws_size = BYTE_ORDER__htonll(hef_header.distinct.v1.ccws_size);
    hef_header.distinct.v1.crc = BYTE_ORDER__htonl(hef_header.distinct.v1.crc);

    return HAILO_SUCCESS;
}

hailo_status Hef::Impl::fill_v2_hef_header(hef__header_t &hef_header, std::shared_ptr<SeekableBytesReader> hef_reader)
{
    auto status = hef_reader->read(reinterpret_cast<uint8_t*>(&hef_header.distinct), sizeof(hef__header_distinct_t::v2));
    CHECK_SUCCESS(status);

    hef_header.distinct.v2.ccws_size = BYTE_ORDER__htonll(hef_header.distinct.v2.ccws_size);
    hef_header.distinct.v2.xxh3_64bits = BYTE_ORDER__htonll(hef_header.distinct.v2.xxh3_64bits);

    return HAILO_SUCCESS;
}

hailo_status Hef::Impl::fill_v3_hef_header(hef__header_t &hef_header, std::shared_ptr<SeekableBytesReader> hef_reader)
{
    auto status = hef_reader->read(reinterpret_cast<uint8_t*>(&hef_header.distinct), sizeof(hef__header_distinct_t::v3));
    CHECK_SUCCESS(status);

    hef_header.distinct.v3.ccws_size_with_padding = BYTE_ORDER__htonll(hef_header.distinct.v3.ccws_size_with_padding);
    hef_header.distinct.v3.xxh3_64bits = BYTE_ORDER__htonll(hef_header.distinct.v3.xxh3_64bits);
    hef_header.distinct.v3.hef_padding_size = BYTE_ORDER__htonl(hef_header.distinct.v3.hef_padding_size);
    hef_header.distinct.v3.additional_info_size = BYTE_ORDER__htonll(hef_header.distinct.v3.additional_info_size);
    hef_header.distinct.v3.proto_xxh3_64bits = BYTE_ORDER__htonll(hef_header.distinct.v3.proto_xxh3_64bits);

    return HAILO_SUCCESS;
}

hailo_status Hef::Impl::fill_core_ops_and_networks_metadata(uint32_t hef_version, std::shared_ptr<SeekableBytesReader> hef_reader, size_t ccws_offset)
{
    fill_core_ops();

    auto status = fill_networks_metadata(hef_version, hef_reader, ccws_offset);
    CHECK_SUCCESS(status);

    // Must be called after fill_networks_metadata
    status = validate_hef_extensions();
    CHECK_SUCCESS(status);

    return HAILO_SUCCESS;
}

hailo_status Hef::Impl::parse_hef_file(const std::string &hef_path)
{
    TRY(auto hef_reader, SeekableBytesReader::create_reader(hef_path));
    auto status = hef_reader->open();
    CHECK_SUCCESS(status);
    m_hef_reader = hef_reader;

    TRY(auto hef_header, parse_hef_header_before_distinct(hef_reader));
    init_hef_version(hef_header.version);

    m_offset_zero_point = 0; // Not relevant for HEADER_VERSION_0
    switch (hef_header.version) {
    case HEADER_VERSION_0: {
        status = hef_reader->read(reinterpret_cast<uint8_t*>(&hef_header.distinct), sizeof(hef__header_distinct_t::v0));
        CHECK_SUCCESS(status);
        MD5_SUM_t calculated_md5 = {};
        status = calc_istream_md5(*hef_reader->get_fstream(), calculated_md5);
        CHECK_SUCCESS(status);
        TRY(const auto hef_file_residue_size, hef_reader->calculate_remaining_size());
        status = validate_hef_header(hef_header, calculated_md5, hef_file_residue_size);
        CHECK_SUCCESS(status);
        init_md5(calculated_md5);
        break;
    }
    case HEADER_VERSION_1: {
        status = fill_v1_hef_header(hef_header, hef_reader);
        CHECK_SUCCESS(status);
        m_offset_zero_point = HEF_HEADER_SIZE_V1 + hef_header.hef_proto_size;
        TRY(auto calculated_residue_size, calc_hef_residue_size(hef_reader, hef_header.version));
        TRY(auto calculated_crc, CRC32::calc_crc_on_stream(hef_reader->get_fstream(), calculated_residue_size));
        status = validate_hef_header(hef_header, calculated_crc, calculated_residue_size);
        CHECK_SUCCESS(status);
        init_crc(calculated_crc);
        break;
    }
    case HEADER_VERSION_2: {
        status = fill_v2_hef_header(hef_header, hef_reader);
        CHECK_SUCCESS(status);
        m_offset_zero_point = HEF_HEADER_SIZE_V2 + hef_header.hef_proto_size;
        TRY(auto calculated_residue_size, calc_hef_residue_size(hef_reader, hef_header.version));
        TRY(auto calculated_xxh3_64bits, Xxhash::calc_xxh3_on_stream(hef_reader->get_fstream(), calculated_residue_size));
        status = validate_hef_header(hef_header, calculated_xxh3_64bits, calculated_residue_size);
        CHECK_SUCCESS(status);
        m_xxh3_64bits = calculated_xxh3_64bits;
        break;
    }
    case HEADER_VERSION_3: {
        status = fill_v3_hef_header(hef_header, hef_reader);
        CHECK_SUCCESS(status);
        m_ccws_section_size = hef_header.distinct.v3.ccws_size_with_padding - hef_header.distinct.v3.hef_padding_size;
        m_offset_zero_point = HEF_HEADER_SIZE_V3 + hef_header.hef_proto_size + hef_header.distinct.v3.hef_padding_size;
        if (0 != hef_header.distinct.v3.proto_xxh3_64bits) {
            // If proto_xxh3_64bits is populated check only it, and let the rest of the HEF be validated later (CCW - on FW, external resources - hef parsing)
            TRY(auto hef_proto_checksum, Xxhash::calc_xxh3_on_stream(hef_reader->get_fstream(), hef_header.hef_proto_size));
            CHECK(hef_header.distinct.v3.proto_xxh3_64bits == hef_proto_checksum, HAILO_HEF_FILE_CORRUPTED, "HEF proto xxhash does not match");
            m_xxh3_64bits = hef_header.distinct.v3.xxh3_64bits;
        } else {
            TRY(auto calculated_residue_size, calc_hef_residue_size(hef_reader, hef_header.version));
            TRY(auto calculated_xxh3_64bits, Xxhash::calc_xxh3_on_stream(hef_reader->get_fstream(), calculated_residue_size));
            status = validate_hef_header(hef_header, calculated_xxh3_64bits, calculated_residue_size);
            CHECK_SUCCESS(status);
            m_xxh3_64bits = calculated_xxh3_64bits;
        }
        break;
    }
    default:
        LOGGER__ERROR("Unsupported hef version {}", hef_header.version);
        return HAILO_HEF_NOT_SUPPORTED;
    }

    // Create arena for faster protobuf parsing
    m_arena = std::make_unique<google::protobuf::Arena>();

    ProtoHEFHef* hef_message = google::protobuf::Arena::CreateMessage<ProtoHEFHef>(m_arena.get());
    google::protobuf::io::IstreamInputStream zero_copy_input(hef_reader->get_fstream().get());
    auto rb = hef_message->ParseFromBoundedZeroCopyStream(&zero_copy_input, hef_header.hef_proto_size); // This line corrupts the file
    CHECK(rb, HAILO_HEF_FILE_CORRUPTED, "Failed parsing HEF file");
    hef_reader->get_fstream()->clear(); // The call to ParseFromBoundedZeroCopyStream might corrupt the file, so we need to clear it's error flags

    // TODO: Remove this reset after stopping support for V0 (in the new format (V1), the file is not corrupted after parsing the protobuf message).
    status = capture_protobuf_references(*hef_message);
    CHECK_SUCCESS(status);

    status = fill_core_ops_and_networks_metadata(hef_header.version, hef_reader, m_offset_zero_point);
    CHECK_SUCCESS(status);

    status = hef_reader->close();
    CHECK_SUCCESS(status);
    TRACE(HefLoadedTrace, hef_path, m_header.sdk_version_str(), m_md5);

    return HAILO_SUCCESS;
}

hailo_status Hef::Impl::parse_hef_memview_internal(const size_t proto_size, const uint8_t *proto_buffer, const uint32_t hef_version,
    std::shared_ptr<SeekableBytesReader> hef_reader, size_t ccws_offset)
{
    // Create arena for faster protobuf parsing
    m_arena = std::make_unique<google::protobuf::Arena>();

    ProtoHEFHef* hef_message = google::protobuf::Arena::CreateMessage<ProtoHEFHef>(m_arena.get());
    auto rb = hef_message->ParseFromArray(proto_buffer, static_cast<int>(proto_size));
    CHECK(rb, HAILO_HEF_FILE_CORRUPTED, "Failed parsing HEF buffer");

    auto status = capture_protobuf_references(*hef_message);
    CHECK_SUCCESS(status);

    status = fill_core_ops_and_networks_metadata(hef_version, hef_reader, ccws_offset);
    CHECK_SUCCESS(status);

    return HAILO_SUCCESS;
}

hailo_status Hef::Impl::parse_hef_memview(const MemoryView &hef_memview)
{
    TRY(auto hef_reader, SeekableBytesReader::create_reader(hef_memview));
    m_hef_reader = hef_reader;

    TRY(auto hef_header, parse_hef_header_before_distinct(hef_reader));
    init_hef_version(hef_header.version);

    CHECK(hef_memview.size() >= sizeof(hef__header_t), HAILO_HEF_FILE_CORRUPTED, "Invalid HEF header");

    m_offset_zero_point = 0; // Not relevant for HEADER_VERSION_0
    switch (hef_header.version) {
    case HEADER_VERSION_0: {
        auto status = hef_reader->read(reinterpret_cast<uint8_t*>(&hef_header.distinct), sizeof(hef__header_distinct_t::v0));
        CHECK_SUCCESS(status);

        auto proto_buffer = (hef_memview.data() + HEF_HEADER_SIZE_V0);
        auto proto_size = (hef_memview.size() - HEF_HEADER_SIZE_V0);

        MD5_SUM_t calculated_md5 = {};
        status = calc_buffer_md5(proto_buffer, proto_size, calculated_md5);
        CHECK_SUCCESS(status);

        status = validate_hef_header(hef_header, calculated_md5, proto_size);
        CHECK_SUCCESS(status);

        init_md5(calculated_md5);

        return parse_hef_memview_internal(proto_size, proto_buffer, hef_header.version, hef_reader, m_offset_zero_point);
    }
    case HEADER_VERSION_1: {
        auto status = fill_v1_hef_header(hef_header, hef_reader);
        CHECK_SUCCESS(status);

        auto proto_and_ccw_buffer = hef_memview.data() + HEF_HEADER_SIZE_V1;
        auto proto_size = hef_memview.size() - HEF_HEADER_SIZE_V1 - hef_header.distinct.v1.ccws_size;

        m_offset_zero_point = HEF_HEADER_SIZE_V1 + hef_header.hef_proto_size;

        TRY(auto proto_and_ccws_size, calc_hef_residue_size(hef_reader, hef_header.version));
        auto proto_and_ccws_buffer = MemoryView::create_const(hef_memview.data() + HEF_HEADER_SIZE_V1, proto_and_ccws_size);
        TRY(auto calculated_crc, CRC32::calc_crc_on_buffer(proto_and_ccws_buffer));

        status = validate_hef_header(hef_header, calculated_crc, proto_and_ccws_size);
        CHECK_SUCCESS(status);

        init_crc(calculated_crc);

        return parse_hef_memview_internal(static_cast<size_t>(proto_size), proto_and_ccw_buffer, hef_header.version, hef_reader, m_offset_zero_point);
    }
    case HEADER_VERSION_2: {
        auto status = fill_v2_hef_header(hef_header, hef_reader);
        CHECK_SUCCESS(status);

        auto proto_and_ccw_buffer = hef_memview.data() + HEF_HEADER_SIZE_V2;
        auto proto_size = hef_memview.size() - HEF_HEADER_SIZE_V2 - hef_header.distinct.v2.ccws_size;

        m_offset_zero_point = HEF_HEADER_SIZE_V2 + hef_header.hef_proto_size;

        TRY(auto proto_and_ccws_size, calc_hef_residue_size(hef_reader, hef_header.version));
        auto proto_and_ccws_buffer = MemoryView::create_const(hef_memview.data() + HEF_HEADER_SIZE_V2, proto_and_ccws_size);
        TRY(auto calculated_xxh3_64bits, Xxhash::calc_xxh3_on_buffer(proto_and_ccws_buffer));

        status = validate_hef_header(hef_header, calculated_xxh3_64bits, proto_and_ccws_size);
        CHECK_SUCCESS(status);
        m_xxh3_64bits = calculated_xxh3_64bits;

        return parse_hef_memview_internal(static_cast<size_t>(proto_size), proto_and_ccw_buffer, hef_header.version, hef_reader, m_offset_zero_point);
    }
    case HEADER_VERSION_3:
    {
        auto status = fill_v3_hef_header(hef_header, hef_reader);
        CHECK_SUCCESS(status);

        auto proto_and_ccw_buffer = hef_memview.data() + HEF_HEADER_SIZE_V3;
        auto proto_size = hef_header.hef_proto_size;

        CHECK(hef_header.distinct.v3.ccws_size_with_padding >= hef_header.distinct.v3.hef_padding_size, HAILO_HEF_FILE_CORRUPTED,
            "Invalid HEF - ccws size is smaller than padding size");
        m_ccws_section_size = hef_header.distinct.v3.ccws_size_with_padding - hef_header.distinct.v3.hef_padding_size;
        m_offset_zero_point = HEF_HEADER_SIZE_V3 + hef_header.hef_proto_size + hef_header.distinct.v3.hef_padding_size;

        if (0 != hef_header.distinct.v3.proto_xxh3_64bits) {
            // If proto_xxh3_64bits is populated check only it, and let the rest of the HEF be validated later (CCW - on FW, external resources - hef parsing)
            auto proto_buffer = MemoryView::create_const(hef_memview.data() + HEF_HEADER_SIZE_V3, hef_header.hef_proto_size);
            TRY(auto hef_proto_checksum, Xxhash::calc_xxh3_on_buffer(proto_buffer));
            CHECK(hef_header.distinct.v3.proto_xxh3_64bits == hef_proto_checksum, HAILO_HEF_FILE_CORRUPTED, "HEF proto xxhash does not match");
            m_xxh3_64bits = hef_header.distinct.v3.xxh3_64bits;
        } else {
            TRY(auto proto_and_ccws_size, calc_hef_residue_size(hef_reader, hef_header.version));
            auto proto_and_ccws_buffer = MemoryView::create_const(hef_memview.data() + HEF_HEADER_SIZE_V3, proto_and_ccws_size);
            TRY(auto calculated_xxh3_64bits, Xxhash::calc_xxh3_on_buffer(proto_and_ccws_buffer));

            status = validate_hef_header(hef_header, calculated_xxh3_64bits, proto_and_ccws_size);
            CHECK_SUCCESS(status);
            m_xxh3_64bits = calculated_xxh3_64bits;
        }

        return parse_hef_memview_internal(static_cast<size_t>(proto_size), proto_and_ccw_buffer, hef_header.version, hef_reader, m_offset_zero_point);
    }
    default:
        LOGGER__ERROR("Unsupported hef version {}", hef_header.version);
        return HAILO_HEF_NOT_SUPPORTED;
    }
}


bool is_multi_layout(const ProtoHEFHwArch &hw_arch) {
    return (hw_arch == ProtoHEFHwArch::PROTO__HW_ARCH__HAILO8L) || (hw_arch == ProtoHEFHwArch::PROTO__HW_ARCH__HAILO15M);
}

hailo_status Hef::Impl::fill_networks_metadata(uint32_t hef_version, std::shared_ptr<SeekableBytesReader> hef_reader, size_t ccws_offset)
{
    fill_extensions_bitset();

    CoreOpMetadataPerArch core_op_metadata;
    uint32_t partial_clusters_layout_bitmap = 0;

    for (auto &network_group : m_groups) {
        // Prepare core_op_metadata
        auto network_group_name = HefUtils::get_network_group_name(*network_group, m_supported_features);
        // TODO: keep metadata per core_op (HRT-9551)
        const auto &core_ops = m_core_ops_per_group[network_group_name];
        assert(core_ops.size() == 1);
        const auto &core_op = core_ops[0];

        // TODO: Clean this code after hef.proto refactor
        std::vector<std::string> sorted_network_names;
        if (m_supported_features.multi_network_support) {
            if (0 != network_group->networks_names_size()) {
                sorted_network_names.reserve(core_op.networks_names.size());
                for (auto &partial_network_name : core_op.networks_names) {
                    auto network_name = HefUtils::get_network_name(network_group_name, partial_network_name);
                    sorted_network_names.push_back(network_name);
                }
            } else if (0 != network_group->partial_network_groups_size()) {
                sorted_network_names.reserve(network_group->partial_network_groups().begin()->network_group().networks_names_size());
                for (auto &partial_network_name : network_group->partial_network_groups().begin()->network_group().networks_names()) {
                    auto network_name = HefUtils::get_network_name(network_group_name, partial_network_name);
                    sorted_network_names.push_back(network_name);
                }
            }
        }
        if (sorted_network_names.empty()) {
            sorted_network_names.push_back(HailoRTDefaults::get_network_name(network_group_name));
        }

        if (is_multi_layout(get_device_arch())) {
            if (m_supported_features.hailo_net_flow) {
                for (auto &partial_core_op : core_op.partial_core_ops) {
                    partial_clusters_layout_bitmap = partial_core_op->layout.partial_clusters_layout_bitmap();
                    TRY(const auto metadata_per_arch,
                        create_metadata_per_arch(*(partial_core_op->core_op), sorted_network_names, hef_version, hef_reader, ccws_offset));
                    TRY(const auto ops_metadata, create_ops_metadata(*network_group, *metadata_per_arch));
                    m_post_process_ops_metadata_per_group.insert({metadata_per_arch->core_op_name(), ops_metadata});
                    core_op_metadata.add_metadata(metadata_per_arch, partial_clusters_layout_bitmap);
                }
            } else {
                for (auto &partial_network_group : network_group->partial_network_groups()) {
                    partial_clusters_layout_bitmap = partial_network_group.layout().partial_clusters_layout_bitmap();
                    ProtoHEFCoreOpMock partial_core_op{
                        partial_network_group.network_group().network_group_metadata(),
                        partial_network_group.network_group().preliminary_config(),
                        partial_network_group.network_group().contexts(),
                        partial_network_group.network_group().sorted_outputs_order(),
                        partial_network_group.network_group().fused_layers_metadata(),
                        partial_network_group.network_group().networks_names(),
                        {}
                    };

                    TRY(const auto metadata_per_arch, create_metadata_per_arch(partial_core_op, sorted_network_names, hef_version, hef_reader, ccws_offset));

                    std::vector<net_flow::PostProcessOpMetadataPtr> empty_metadata_ops;
                    m_post_process_ops_metadata_per_group.insert({metadata_per_arch->core_op_name(), empty_metadata_ops});
                    core_op_metadata.add_metadata(metadata_per_arch, partial_clusters_layout_bitmap);
                }
            }
        } else {
            partial_clusters_layout_bitmap = PARTIAL_CLUSTERS_LAYOUT_IGNORE;
            TRY(const auto metadata_per_arch, create_metadata_per_arch(core_op, sorted_network_names, hef_version, hef_reader, ccws_offset));
            TRY(auto ops_metadata, create_ops_metadata(*network_group, *metadata_per_arch));
            m_post_process_ops_metadata_per_group.insert({metadata_per_arch->core_op_name(), ops_metadata});
            core_op_metadata.add_metadata(metadata_per_arch, partial_clusters_layout_bitmap);
        }

        // Taking the full-layout's name (name is same across all layouts)
        TRY(const auto metadata, core_op_metadata.get_metadata(PARTIAL_CLUSTERS_LAYOUT_IGNORE));
        auto core_op_name = metadata->core_op_name();
        std::map<std::string, CoreOpMetadataPerArch> core_op_metadata_map;
        core_op_metadata_map[core_op_name] = core_op_metadata;
        // Prepare network_group_metadata
        CHECK(!contains(m_network_group_metadata, network_group_name),
            HAILO_INVALID_OPERATION, "Network group with the name {} is already configured on the device", network_group_name);

        // TODO: Clean this code after hef.proto refactor
        std::vector<std::string> sorted_output_names;
        if (core_op.fused_layers_metadata.network_has_fused_layers()) {
            // If the model has fused layers, updated sorted_output_names is under the fused layer metadata
            for (auto &name : core_op.fused_layers_metadata.updated_sorted_output_names()) {
                sorted_output_names.push_back(name);
            }
        } else if(!m_supported_features.hailo_net_flow && (0 != network_group->partial_network_groups_size()) &&
            (network_group->partial_network_groups().begin()->network_group().sorted_outputs_order_size())) {
            // If the model doesnt support net_flow, its possible that sorted output names will be under the partial_network_groups metadata
            for (auto &name : network_group->partial_network_groups().begin()->network_group().sorted_outputs_order()) {
                sorted_output_names.push_back(name);
            }
        } else if (0 != network_group->sorted_outputs_order_size()) {
            // Most cases should fall here - either net_flow is supported, or network_group->sorted_outputs_order() has values
            const auto &names = IS_PP_DISABLED() ? network_group->fused_layers_metadata().updated_sorted_output_names() :
                network_group->sorted_outputs_order();
            for (const auto &name : names) {
                sorted_output_names.push_back(name);
            }
        } else {
            // For very old HEFs, sorted_output_names might be in the last context's metadata
            uint32_t number_of_contexts = core_op.contexts.size();
            const auto& context_metadata = core_op.contexts[number_of_contexts - 1].metadata();
            CHECK(0 < context_metadata.sorted_outputs_order_size(), HAILO_INVALID_HEF,
                "Sorted output names is not set up in the HEF.");
            for (auto &name : context_metadata.sorted_outputs_order()) {
                sorted_output_names.push_back(name);
            }
        }

        std::vector<net_flow::PostProcessOpMetadataPtr> empty_ops_metadata;
        auto &ops_metadata = IS_PP_DISABLED() ? empty_ops_metadata : m_post_process_ops_metadata_per_group.at(network_group_name);
        TRY(auto network_group_metadata, NetworkGroupMetadata::create(network_group_name, std::move(core_op_metadata_map),
            sorted_output_names, m_supported_features, sorted_network_names, ops_metadata));
        m_network_group_metadata.emplace(network_group_name, std::move(network_group_metadata));
    }
    return HAILO_SUCCESS;
}

static Expected<std::vector<ConfigChannelInfo>> parse_config_channels_info(const ProtoHEFCoreOpMock &core_op)
{
    const auto &metadata = core_op.network_group_metadata;
    // Backwards compatibility for HEFs without the cfg_channels_count field
    CHECK_AS_EXPECTED(IS_FIT_IN_UINT8(metadata.cfg_channels_count()),
        HAILO_INVALID_HEF, "Invalid cfg channels count");
    const uint8_t cfg_channels_count = (0 == metadata.cfg_channels_count()) ?
        1 : static_cast<uint8_t>(metadata.cfg_channels_count());


    std::vector<ConfigChannelInfo> config_channels_info;
    config_channels_info.reserve(cfg_channels_count);
    const auto &cfg_channels_config = metadata.cfg_channels_config();
    for (uint8_t config_stream_index = 0; config_stream_index < cfg_channels_count; config_stream_index++) {
        auto cfg_info = std::find_if(cfg_channels_config.begin(), cfg_channels_config.end(),
            [config_stream_index](const auto &cfg_info)
            {
                return cfg_info.cfg_channel_index() == config_stream_index;
            });

        if (cfg_info != cfg_channels_config.end()) {
            CHECK_AS_EXPECTED(IS_FIT_IN_UINT8(cfg_info->engine_id()), HAILO_INVALID_HEF, "Invalid dma engine index");
            config_channels_info.emplace_back(ConfigChannelInfo{static_cast<uint8_t>(cfg_info->engine_id())});
        }
        else {
            // Not found - can happen on old HEF or hailo8. In those case we want to use the default engine
            config_channels_info.emplace_back(ConfigChannelInfo{vdma::DEFAULT_ENGINE_INDEX});
        }
    }

    return config_channels_info;
}

Expected<CoreOpMetadataPtr> Hef::Impl::create_metadata_per_arch(const ProtoHEFCoreOpMock &core_op, const std::vector<std::string> &sorted_network_names,
    uint32_t hef_version, std::shared_ptr<SeekableBytesReader> hef_reader, size_t ccws_offset)
{
    const auto &ng_name = core_op.network_group_metadata.network_group_name();
    // TODO: validate that there's a read+write layer for each cache + no cache_id is only read or written without the
    //       other. They can be across different contexts (HRT-13655)
    TRY(auto preliminary_context, HefUtils::parse_preliminary_context(core_op.preliminary_config, m_supported_features, hef_version, hef_reader, ccws_offset));
    TRY_V(auto dynamic_contexts, HefUtils::parse_dynamic_contexts(core_op, m_supported_features, get_device_arch(), hef_version, hef_reader, ccws_offset));
    TRY(auto config_channels_info,  parse_config_channels_info(core_op));

    // If const input layer is found in the preliminary context, or first dynamic context we can't use fast batch switch
    const auto can_fast_batch_switch =
        !(preliminary_context.const_input_layer_found() || dynamic_contexts[0].const_input_layer_found());

    // Currently, CoreOp name is the same as network_group_name, thats why we init it with it.
    // TODO: HRT-9551 - Change it when supporting multi core ops.
    auto metadata_per_arch = make_shared_nothrow<CoreOpMetadata>(ng_name,
        std::move(preliminary_context), std::move(dynamic_contexts), std::move(config_channels_info),
        m_supported_features, sorted_network_names, can_fast_batch_switch);
    CHECK_NOT_NULL_AS_EXPECTED(metadata_per_arch, HAILO_OUT_OF_HOST_MEMORY);

    return metadata_per_arch;
}

void Hef::Impl::fill_core_ops()
{
    if (m_supported_features.hailo_net_flow) {
        for (const auto &net_group : m_groups) {
            auto core_op_iter = std::find_if(net_group->ops().begin(), net_group->ops().end(),
                [](auto &op) {
                    return op.op_case() == ProtoHEFOp::kCoreOp;
                });
            assert(core_op_iter != m_groups[0]->ops().end());
            std::vector<std::shared_ptr<ProtoHEFPartialCoreOpMock>> partial_core_ops;
            partial_core_ops.reserve(core_op_iter->core_op().partial_core_ops().size());
            for (auto &partial_core_op : core_op_iter->core_op().partial_core_ops()) {
                ProtoHEFCoreOpMock core_op{
                    partial_core_op.core_op().network_group_metadata(),
                    partial_core_op.core_op().preliminary_config(),
                    partial_core_op.core_op().contexts(),
                    partial_core_op.core_op().sorted_outputs_order(),
                    partial_core_op.core_op().fused_layers_metadata(),
                    partial_core_op.core_op().networks_names(),
                    {}
                };
                ProtoHEFPartialCoreOpMock partial_core_op_mock{
                    std::make_shared<ProtoHEFCoreOpMock>(core_op),
                    partial_core_op.layout()
                };
                partial_core_ops.push_back(std::make_shared<ProtoHEFPartialCoreOpMock>(partial_core_op_mock));
            }
            ProtoHEFCoreOpMock core_op{
                core_op_iter->core_op().network_group_metadata(),
                core_op_iter->core_op().preliminary_config(),
                core_op_iter->core_op().contexts(),
                core_op_iter->core_op().sorted_outputs_order(),
                core_op_iter->core_op().fused_layers_metadata(),
                core_op_iter->core_op().networks_names(),
                partial_core_ops
            };
            auto net_group_name = HefUtils::get_network_group_name(*net_group, m_supported_features);
            m_core_ops_per_group[net_group_name].push_back(std::move(core_op));
        }
    } else {
        for (const auto &net_group : m_groups) {
            std::vector<std::shared_ptr<ProtoHEFPartialCoreOpMock>> partial_core_ops;
            partial_core_ops.reserve(net_group->partial_network_groups().size());
            for (auto &partial_network_group : net_group->partial_network_groups()) {
                ProtoHEFCoreOpMock core_op{
                    partial_network_group.network_group().network_group_metadata(),
                    partial_network_group.network_group().preliminary_config(),
                    partial_network_group.network_group().contexts(),
                    partial_network_group.network_group().sorted_outputs_order(),
                    partial_network_group.network_group().fused_layers_metadata(),
                    partial_network_group.network_group().networks_names(),
                    {}
                };
                ProtoHEFPartialCoreOpMock partial_core_op{
                    std::make_shared<ProtoHEFCoreOpMock>(core_op),
                    partial_network_group.layout()
                };
                partial_core_ops.push_back(std::make_shared<ProtoHEFPartialCoreOpMock>(partial_core_op));
            }
            ProtoHEFCoreOpMock core_op{
                net_group->network_group_metadata(),
                net_group->preliminary_config(),
                net_group->contexts(),
                net_group->sorted_outputs_order(),
                net_group->fused_layers_metadata(),
                net_group->networks_names(),
                partial_core_ops
            };
            auto net_group_name = HefUtils::get_network_group_name(*net_group, m_supported_features);
            m_core_ops_per_group[net_group_name].push_back(std::move(core_op));
        }
    }
}

hailo_status Hef::Impl::capture_protobuf_references(ProtoHEFHef &hef_message)
{
    // Capture raw pointers to arena-allocated protobuf objects
    // Arena (m_arena) owns the memory lifetime
    m_groups.reserve(hef_message.network_groups().size());
    for (int i = 0; i < hef_message.network_groups().size(); i++) {
        ProtoHEFNetworkGroup* network_group = hef_message.mutable_network_groups(i);
        CHECK(nullptr != network_group, HAILO_INTERNAL_FAILURE, "Null network group found while parsing HEF; Unexpected");
        m_groups.emplace_back(network_group);
    }

    m_hef_extensions.reserve(hef_message.extensions().size());
    for (const auto &extension : hef_message.extensions()) {
        m_hef_extensions.emplace_back(extension);
    }

    m_header.CopyFrom(hef_message.header());
    m_included_features.CopyFrom(hef_message.included_features());

    m_hef_optional_extensions.reserve(hef_message.optional_extensions().size());
    for (const auto &optional_extension : hef_message.optional_extensions()) {
        m_hef_optional_extensions.emplace_back(optional_extension);
    }

    m_supported_features = get_supported_features(m_header, m_hef_extensions, m_included_features,
        m_hef_optional_extensions);

    for (const auto &external_resouce : hef_message.external_resources()) {
        ExternalResourceInfo external_resource_info{external_resouce.name(), external_resouce.size(), external_resouce.offset(), external_resouce.xxhash()};
        m_hef_external_resources.emplace(external_resouce.name(), external_resource_info);
    }

    return HAILO_SUCCESS;
}

Expected<std::shared_ptr<Buffer>> Hef::Impl::get_hef_as_buffer()
{
    if (m_hef_buffer) {
        auto ptr = m_hef_buffer;
        return ptr;
    }

    auto hef_reader = get_hef_reader();
    CHECK_SUCCESS_AS_EXPECTED(hef_reader->open());
    TRY(auto size, hef_reader->get_size());
    TRY(auto buffer_ptr, Buffer::create_shared(size, BufferStorageParams::create_dma()));

    CHECK_SUCCESS(hef_reader->read(buffer_ptr->data(), size));
    CHECK_SUCCESS(hef_reader->close());
    return buffer_ptr;
}

Hef::Impl::Impl(const std::string &hef_path, hailo_status &status) : m_zero_copy_config_over_descs(false)
{
    status = HAILO_UNINITIALIZED;
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    status = parse_hef_file(hef_path);
    if (HAILO_SUCCESS != status) {
        LOGGER__ERROR("Failed parsing HEF file");
        return;
    }

    status = HAILO_SUCCESS;
}

Hef::Impl::Impl(std::shared_ptr<Buffer> hef_buffer, hailo_status &status) : m_zero_copy_config_over_descs(false)
{
    status = HAILO_UNINITIALIZED;
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    m_hef_buffer = hef_buffer;

    status = parse_hef_memview(MemoryView(*m_hef_buffer));
    if (HAILO_SUCCESS != status) {
        LOGGER__ERROR("Failed parsing HEF buffer");
        return;
    }

    status = HAILO_SUCCESS;
}

void Hef::Impl::fill_extensions_bitset()
{
    for (auto extension : SUPPORTED_EXTENSIONS) {
        m_supported_extensions_bitset[extension] = 1;
    }
}

SupportedFeatures Hef::Impl::get_supported_features(const ProtoHEFHeader &header,
        const std::vector<ProtoHEFExtension> &hef_extensions, const ProtoHEFIncludedFeatures &included_features,
        const std::vector<ProtoHEFOptionalExtension> &hef_optional_extensions)
{
    SupportedFeatures supported_features{};
    supported_features.padded_ddr_buffers = check_hef_extension(ProtoHEFExtensionType::PADDED_DDR_BUFFERS,
        header, hef_extensions, included_features);
    supported_features.multi_network_support = check_hef_optional_extension(ProtoHEFExtensionType::MULTI_NETWORK_VARIABLE_BATCH_SIZE,
        header, hef_optional_extensions);
    supported_features.multi_context = check_hef_extension(ProtoHEFExtensionType::IS_MULTI_CONTEXTS,
        header, hef_extensions, included_features);
    supported_features.preliminary_run_asap = check_hef_extension(ProtoHEFExtensionType::KO_RUN_ASAP,
        header, hef_extensions, included_features);
    supported_features.hailo_net_flow = check_hef_extension(ProtoHEFExtensionType::HAILO_NET_FLOW,
        header, hef_extensions, included_features);
    supported_features.dual_direction_stream_index = check_hef_extension(ProtoHEFExtensionType::DUAL_DIRECTION_STREAM_INDEX,
        header, hef_extensions, included_features);
    supported_features.nms_burst_mode = check_hef_extension(ProtoHEFExtensionType::NMS_OUTPUT_BURST,
        header, hef_extensions, included_features);
    supported_features.output_scale_by_feature = check_hef_extension(ProtoHEFExtensionType::OUTPUT_SCALE_PER_FEATURE,
        header, hef_extensions, included_features);
    supported_features.periph_calculation_in_hailort = check_hef_extension(ProtoHEFExtensionType::PERIPH_CALCULATION_IN_HAILORT,
        header, hef_extensions, included_features);
    supported_features.core_hw_padding_config_in_dfc = check_hef_optional_extension(ProtoHEFExtensionType::HW_PADDING,
        header, hef_optional_extensions);
    supported_features.batch_register_config = check_hef_extension(ProtoHEFExtensionType::BATCH_REGISTER_CONFIG,
        header, hef_extensions, included_features);
    supported_features.shared_config = check_hef_extension(ProtoHEFExtensionType::SHARED_CONFIG,
        header, hef_extensions, included_features);
    supported_features.split_allow_input_action = check_hef_extension(ProtoHEFExtensionType::ENABLE_CONFIG_CHANNELS,
        header, hef_extensions, included_features);

    return supported_features;
}

net_flow::NmsPostProcessConfig create_post_process_nms_config(const ProtoHEFOp &op_proto)
{
    net_flow::NmsPostProcessConfig nms_config{};
    nms_config.nms_score_th = (float32_t)op_proto.nms_op().nms_score_th();
    nms_config.nms_iou_th = (float32_t)op_proto.nms_op().nms_iou_th();
    nms_config.max_proposals_per_class = op_proto.nms_op().max_proposals_per_class();
    nms_config.number_of_classes = op_proto.nms_op().classes();
    nms_config.max_proposals_total = nms_config.max_proposals_per_class * nms_config.number_of_classes;
    nms_config.background_removal = op_proto.nms_op().background_removal();
    nms_config.background_removal_index = op_proto.nms_op().background_removal_index();
    nms_config.bbox_only = op_proto.nms_op().bbox_decoding_only();

    return nms_config;
}

Expected<net_flow::YoloPostProcessConfig> create_yolov5_config(const google::protobuf::RepeatedPtrField<ProtoHEFYoloBboxDecoder> &bbox_decoders,
    double image_height, double image_width, const std::map<size_t, LayerInfo> &pad_index_to_streams_info)
{
    net_flow::YoloPostProcessConfig yolo_config{};
    yolo_config.image_height = static_cast<float32_t>(image_height);
    yolo_config.image_width = static_cast<float32_t>(image_width);
    for (auto &bbox_proto : bbox_decoders) {
        std::vector<int> bbox_anchors;
        CHECK_AS_EXPECTED((bbox_proto.h().size() == bbox_proto.w().size()), HAILO_INVALID_HEF,
            "YOLOv5 height anchors count {} doesn't mach the width anchors count {}", bbox_proto.h().size(), bbox_proto.w().size());
        for (int i = 0; i < bbox_proto.h().size(); ++i) {
            bbox_anchors.push_back(bbox_proto.w()[i]);
            bbox_anchors.push_back(bbox_proto.h()[i]);
        }
        assert(contains(pad_index_to_streams_info, static_cast<size_t>(bbox_proto.pad_index())));
        yolo_config.anchors.insert({pad_index_to_streams_info.at(bbox_proto.pad_index()).name, bbox_anchors});
    }

    return yolo_config;
}

Expected<std::unordered_map<std::string, net_flow::BufferMetaData>> create_inputs_metadata(const ProtoHEFOp &op_proto,
    const std::map<size_t, LayerInfo> &pad_index_to_streams_info, const std::map<size_t, size_t> &input_to_output_pads)
{
    std::unordered_map<std::string, net_flow::BufferMetaData> inputs_metadata;
    for (auto &input_pad : op_proto.input_pads()) {
        CHECK_AS_EXPECTED(contains(input_to_output_pads, static_cast<size_t>(input_pad.index())), HAILO_INVALID_HEF,
            "NMS op is not connected to core op");
        auto output_pad_index = input_to_output_pads.at(input_pad.index());
        CHECK_AS_EXPECTED(contains(pad_index_to_streams_info, output_pad_index), HAILO_INVALID_HEF,
            "Pad {} of post-process {} is not connected to any core output stream",
                input_pad.index(), op_proto.name());
        const auto &op_input_stream = pad_index_to_streams_info.at(output_pad_index);
        net_flow::BufferMetaData input_metadata{};
        input_metadata.format = op_input_stream.format;
        input_metadata.quant_info = op_input_stream.quant_info;
        input_metadata.shape = op_input_stream.shape;
        input_metadata.padded_shape = op_input_stream.hw_shape;
        inputs_metadata.insert({op_input_stream.name, input_metadata});
    }

    return inputs_metadata;
}

uint32_t compute_num_of_proposals(const std::unordered_map<std::string, net_flow::BufferMetaData> &inputs_metadatas, std::map<std::string, 
    std::vector<int>> &anchors)
{
    uint32_t num_of_proposals = 0;
    for (const auto &input_metadata_pair : inputs_metadatas) {
        auto &name = input_metadata_pair.first;
        auto &input_metadata = input_metadata_pair.second;
        assert(contains(anchors, name));
        auto &layer_anchors = anchors.at(name);
        auto num_of_anchors = net_flow::YOLOv5PostProcessOp::get_num_of_anchors(layer_anchors);
        num_of_proposals += static_cast<uint32_t>(num_of_anchors * input_metadata.shape.height * input_metadata.shape.width);
    }
    return num_of_proposals;
}

Expected<net_flow::PostProcessOpMetadataPtr> create_yolov5_op_metadata(const ProtoHEFOp &op_proto,
    const std::map<size_t, LayerInfo> &pad_index_to_streams_info, const std::map<size_t, size_t> &input_to_output_pads,
    const std::string &network_name)
{
    auto nms_config = create_post_process_nms_config(op_proto);

    TRY(auto yolo_config, create_yolov5_config(op_proto.nms_op().yolo_nms_op().bbox_decoders(),
        op_proto.nms_op().yolo_nms_op().image_height(), op_proto.nms_op().yolo_nms_op().image_width(), pad_index_to_streams_info));
    TRY(auto inputs_metadata, create_inputs_metadata(op_proto, pad_index_to_streams_info, input_to_output_pads));

    std::unordered_map<std::string, net_flow::BufferMetaData> outputs_metadata;
    net_flow::BufferMetaData output_metadata{};
    output_metadata.format = net_flow::NmsOpMetadata::expand_output_format_autos_by_op_type(
        { HAILO_FORMAT_TYPE_AUTO, HAILO_FORMAT_ORDER_