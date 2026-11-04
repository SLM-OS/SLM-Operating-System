# gen_eviction_mlp_weights.cmake - turn the eviction MLP's fp32 weights
# (runtime/src/mm/eviction/generated/mlp_policy_f32.rs) into a C header
# for the cold-boot GPU eviction pipeline (#1092,
# kernel/gpu/nvidia/ga10b_eviction_weights.c).
#
# Usage: cmake -DRS_FILE=<mlp_policy_f32.rs> -DOUT_FILE=<header> -P <this>
#
# Each `pub const NAME: [...] = [ ... ];` array becomes
#     #define GA10B_EVICT_MLP_NAME <comma-separated literals>
#     #define GA10B_EVICT_MLP_NAME_COUNT <n>
# with the literals copied verbatim. The C compiler then rounds each
# decimal to double and the double to float, which is the same path
# scripts/eviction-extract-weights.py takes (float() then
# struct.pack('<f')), so the bytes equal the W_*.bin / B_*.bin files
# the Linux helper loads; kernel/tests/test_ga10b_cold_pipelines.c pins
# them by checksum. Plain CMake: no Python at build time.

if(NOT RS_FILE OR NOT OUT_FILE)
    message(FATAL_ERROR "gen_eviction_mlp_weights: RS_FILE and OUT_FILE are required")
endif()
if(NOT EXISTS "${RS_FILE}")
    message(FATAL_ERROR "gen_eviction_mlp_weights: ${RS_FILE} not found")
endif()

file(READ "${RS_FILE}" _rs)

# name -> expected element count (runtime_mlp.rs L*_IN / L*_OUT).
set(_arrays W_L1 B_L1 W_L2 B_L2 W_L3 B_L3 W_OUT B_OUT)
set(_count_W_L1 1728)
set(_count_B_L1 64)
set(_count_W_L2 2048)
set(_count_B_L2 32)
set(_count_W_L3 512)
set(_count_B_L3 16)
set(_count_W_OUT 16)
set(_count_B_OUT 1)

get_filename_component(_rs_name "${RS_FILE}" NAME)
set(_out "/* Generated from ${_rs_name}\n * by cmake/gen_eviction_mlp_weights.cmake. Do not edit. */\n")
string(APPEND _out "#ifndef GA10B_EVICTION_MLP_WEIGHTS_GEN_H\n#define GA10B_EVICTION_MLP_WEIGHTS_GEN_H\n")

foreach(_name IN LISTS _arrays)
    string(FIND "${_rs}" "pub const ${_name}:" _pos)
    if(_pos LESS 0)
        message(FATAL_ERROR "gen_eviction_mlp_weights: no `pub const ${_name}:` in ${RS_FILE}")
    endif()
    string(SUBSTRING "${_rs}" ${_pos} -1 _tail)
    # Skip the type annotation: the initializer starts after "= ".
    string(FIND "${_tail}" "= [" _eq)
    if(_eq LESS 0)
        message(FATAL_ERROR "gen_eviction_mlp_weights: ${_name} has no `= [` initializer")
    endif()
    math(EXPR _body_start "${_eq} + 2")
    string(SUBSTRING "${_tail}" ${_body_start} -1 _tail)
    string(FIND "${_tail}" "];" _end)
    if(_end LESS 0)
        message(FATAL_ERROR "gen_eviction_mlp_weights: ${_name} initializer is not terminated by `];`")
    endif()
    string(SUBSTRING "${_tail}" 0 ${_end} _body)
    string(REGEX MATCHALL "-?[0-9]+(\\.[0-9]*)?([eE][-+]?[0-9]+)?" _nums "${_body}")
    list(LENGTH _nums _n)
    if(NOT _n EQUAL ${_count_${_name}})
        message(FATAL_ERROR "gen_eviction_mlp_weights: ${_name} has ${_n} values, expected ${_count_${_name}}")
    endif()
    list(JOIN _nums ", " _joined)
    string(APPEND _out "#define GA10B_EVICT_MLP_${_name}_COUNT ${_n}\n")
    string(APPEND _out "#define GA10B_EVICT_MLP_${_name} ${_joined}\n")
endforeach()

string(APPEND _out "#endif\n")

# Rewrite only on change so dependants do not rebuild needlessly.
set(_old "")
if(EXISTS "${OUT_FILE}")
    file(READ "${OUT_FILE}" _old)
endif()
if(NOT _old STREQUAL _out)
    file(WRITE "${OUT_FILE}" "${_out}")
endif()
