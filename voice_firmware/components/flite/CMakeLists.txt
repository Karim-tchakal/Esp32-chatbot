set(FLITE_ROOT "${CMAKE_CURRENT_LIST_DIR}/src")

set(FLITE_SOURCE_DIRS
    audio
    cg
    hrg
    lexicon
    regex
    speech
    stats
    utils
    wavesynth
)
set(FLITE_SOURCES)
foreach(source_dir IN LISTS FLITE_SOURCE_DIRS)
    file(GLOB_RECURSE source_files
         "${FLITE_ROOT}/${source_dir}/*.c")
    list(APPEND FLITE_SOURCES ${source_files})
endforeach()
file(GLOB_RECURSE voice_sources
     "${FLITE_ROOT}/lang/cmu_us_kal/*.c"
     "${FLITE_ROOT}/lang/cmulex/*.c"
     "${FLITE_ROOT}/lang/usenglish/*.c")
list(APPEND FLITE_SOURCES ${voice_sources})
list(APPEND FLITE_SOURCES
     "${FLITE_ROOT}/synth/cst_synth.c")
list(APPEND FLITE_SOURCES
     "${FLITE_ROOT}/synth/cst_ffeatures.c"
     "${FLITE_ROOT}/synth/cst_phoneset.c"
     "${FLITE_ROOT}/synth/cst_ssml.c"
     "${FLITE_ROOT}/synth/cst_utt_utils.c"
     "${FLITE_ROOT}/synth/cst_voice.c"
     "${FLITE_ROOT}/synth/flite.c")
list(FILTER FLITE_SOURCES EXCLUDE REGEX "/au_(alsa|command|oss|palmos|pulseaudio|sun|win|wince)\\.c$")
list(FILTER FLITE_SOURCES EXCLUDE REGEX "/cst_(file_(palmos|wince)|mmap_(posix|win32)|socket)\\.c$")

set(FLITE_TTS_SYMBOL_PREFIXES
    apply_synth_method=flite_tts_apply_synth_method
    apply_synth_module=flite_tts_apply_synth_module
    breakfunc_val=flite_tts_breakfunc_val
    default_lexical_insertion=flite_tts_default_lexical_insertion
    default_textanalysis=flite_tts_default_textanalysis
    default_tokenization=flite_tts_default_tokenization
    default_tokentowords=flite_tts_default_tokentowords
    default_utt_break=flite_tts_default_utt_break
    is_keep_punc=flite_tts_is_keep_punc
    utt_init=flite_tts_utt_init
    utt_synth=flite_tts_utt_synth
    val_breakfunc=flite_tts_val_breakfunc)

idf_component_register(
    SRCS ${FLITE_SOURCES}
    INCLUDE_DIRS
        "${FLITE_ROOT}"
        "${FLITE_ROOT}/include"
        "${FLITE_ROOT}/lang/cmu_us_kal"
        "${FLITE_ROOT}/lang/cmulex"
        "${FLITE_ROOT}/lang/usenglish"
)

# ESP-SR's G2P archive exports overlapping Flite symbols.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    set_source_files_properties("${FLITE_ROOT}/synth/cst_synth.c" PROPERTIES
        COMPILE_DEFINITIONS "${FLITE_TTS_SYMBOL_PREFIXES}")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE CST_NO_SOCKETS CST_NO_MMAP)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-fallthrough
    -Wno-error=absolute-value
)
