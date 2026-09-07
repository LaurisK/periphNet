# ==============================================================================
# Cluster purity — the two constraints that must be checked on OBJECT FILES
# (docs/design_battery_cluster.md §4, constraints 4 and 6)
#
#   4. NO FLOATING POINT ANYWHERE IN App/Cluster.  R4.2 says "anywhere in the
#      aggregation path"; a source grep cannot tell a rule from a sentence
#      describing the rule, and cluster.h's own preamble contains the words
#      "float" and "double".  With -mfloat-abi=hard -mfpu=fpv4-sp-d16 a float
#      op emits a VFP mnemonic (vadd.f32) and a double op emits an __aeabi_d*
#      reference, so the pair catches both — INCLUDING the (float) casts and
#      1.0f literals a declaration-anchored regex misses entirely.
#
#   6. THE CLUSTER NEVER CALLS Pack_Command.  A module that both computes a
#      current limit and can open a contactor has two control authorities over
#      the same cells and nothing arbitrates them.  A comment produces no
#      relocation, so an undefined-symbol check is EXACT.
#
# Invoked as a POST_BUILD step with -DOBJDIR/-DOBJDUMP/-DNM.  A missing object
# directory is not a failure: a configure that has not built yet, or a
# generator laying objects out elsewhere, must not fail the build over a check
# it cannot run — it prints and returns, and the real signal is the build that
# does produce objects.
# ==============================================================================

if(NOT EXISTS "${OBJDIR}")
    message(STATUS "Cluster purity: no objects at ${OBJDIR}; check skipped")
    return()
endif()

file(GLOB _objs "${OBJDIR}/*.obj" "${OBJDIR}/*.o")
if(NOT _objs)
    message(STATUS "Cluster purity: no objects in ${OBJDIR}; check skipped")
    return()
endif()

foreach(_o ${_objs})
    get_filename_component(_name "${_o}" NAME)

    # --- floating point, in the emitted instructions ---
    execute_process(COMMAND "${OBJDUMP}" -d "${_o}"
                    OUTPUT_VARIABLE _dis RESULT_VARIABLE _rc
                    ERROR_QUIET)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "Cluster purity: objdump failed on ${_name}")
    endif()
    # A VFP mnemonic is `v<something>.f32` / `.f64` in the disassembly text.
    if(_dis MATCHES "\t(v[a-z0-9]+)\\.f(32|64)")
        message(FATAL_ERROR
                "Cluster purity: ${_name} emits a floating-point instruction "
                "(${CMAKE_MATCH_1}.f${CMAKE_MATCH_2}).  No float may appear "
                "anywhere in App/Cluster (design §4 constraint 4).")
    endif()

    # --- soft-float doubles, and Pack_Command, in the undefined symbols ---
    execute_process(COMMAND "${NM}" -u "${_o}"
                    OUTPUT_VARIABLE _und RESULT_VARIABLE _rc2
                    ERROR_QUIET)
    if(NOT _rc2 EQUAL 0)
        message(FATAL_ERROR "Cluster purity: nm failed on ${_name}")
    endif()
    if(_und MATCHES "__aeabi_[df]")
        message(FATAL_ERROR
                "Cluster purity: ${_name} references a soft-float helper "
                "(__aeabi_[df]*).  No double may appear anywhere in "
                "App/Cluster (design §4 constraint 4).")
    endif()
    if(_und MATCHES "Pack_Command")
        message(FATAL_ERROR
                "Cluster purity: ${_name} references Pack_Command.  A module "
                "that both computes a limit and can open a contactor has two "
                "control authorities over the same cells and nothing "
                "arbitrates them (design §4 constraint 6).")
    endif()
endforeach()

message(STATUS "Cluster purity: ok")
