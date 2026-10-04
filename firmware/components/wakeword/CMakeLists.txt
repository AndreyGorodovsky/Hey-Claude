# Wake-word detection with a microWakeWord model.
#
# The model is two files in models/: the network itself (.tflite) and a
# manifest (.json) with the settings it was trained and tuned with. The
# settings are read from the manifest here and handed to the code as
# compile-time constants, so the manifest is their only source; changing the
# model means changing WAKEWORD_MODEL below and nothing else. Trained models
# keep their run in the name (hey_claude_run3), so that the boot log and
# `wake` show which run a device test was made with.
set(WAKEWORD_MODEL "hey_claude_run3")

set(manifest_path "${CMAKE_CURRENT_LIST_DIR}/models/${WAKEWORD_MODEL}.json")
file(READ "${manifest_path}" manifest)
# Re-run this step when the manifest changes, not only when this file does
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest_path}")

# string(JSON ... GET) stops the build with an error if a field is missing
string(JSON mww_version GET "${manifest}" version)
string(JSON mww_file GET "${manifest}" model)
string(JSON mww_phrase GET "${manifest}" wake_word)
string(JSON mww_cutoff GET "${manifest}" micro probability_cutoff)
string(JSON mww_window GET "${manifest}" micro sliding_window_size)
string(JSON mww_step GET "${manifest}" micro feature_step_size)
string(JSON mww_arena GET "${manifest}" micro tensor_arena_size)
# Version 2 is the streaming format wakeword.cc is written for
if(NOT mww_version EQUAL 2)
    message(FATAL_ERROR "${manifest_path}: microWakeWord model version ${mww_version}, expected 2")
endif()
# The boot log and `wake` report WAKEWORD_MODEL, but the network embedded is
# the file the manifest names. They must be the same model, or a device test
# would be credited to the wrong training run.
if(NOT mww_file STREQUAL "${WAKEWORD_MODEL}.tflite")
    message(FATAL_ERROR "${manifest_path}: names the model file ${mww_file}, expected ${WAKEWORD_MODEL}.tflite")
endif()

# Embedded under a fixed name, so the symbol the code refers to
# (_binary_wakeword_model_tflite_start) does not depend on which model it is
set(model_copy "${CMAKE_CURRENT_BINARY_DIR}/wakeword_model.tflite")
configure_file("${CMAKE_CURRENT_LIST_DIR}/models/${mww_file}" "${model_copy}" COPYONLY)

idf_component_register(SRCS "wakeword.cc"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_event      # wakeword.h includes esp_event.h
                       PRIV_REQUIRES audio audio_ring
                                     esp-tflite-micro             # runs the model
                                     esp-micro-speech-features    # audio to features
                                     esp_timer                    # timing each run
                                     esp_psram                    # model copy in PSRAM
                       EMBED_FILES "${model_copy}")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WAKEWORD_MODEL_NAME="${WAKEWORD_MODEL}"
    WAKEWORD_PHRASE="${mww_phrase}"
    WAKEWORD_CUTOFF=${mww_cutoff}f
    WAKEWORD_WINDOW=${mww_window}
    WAKEWORD_STEP_MS=${mww_step}
    WAKEWORD_ARENA=${mww_arena})
