cmake_minimum_required(VERSION 3.20)

# Static CRT: players (and the bot) don't need the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(TLDCoop LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The Long Dark is 64-bit; configure from an x64 developer environment.")
endif()

# MinHook, built here so its trampoline builder can be ours (src/minhook_trampoline.c): it jumps back into a hooked
# function past the bytes another mod's hook may overwrite when it hooks the same function after us.
add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/hde/hde64.c
  src/minhook_trampoline.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include PRIVATE third_party/minhook/src)

# Everything below is our code.
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zi)
add_link_options(/DEBUG /OPT:REF /OPT:ICF)

# Protocol + UDP session, shared by the mod and the bot.
add_library(tldnet STATIC src/net/session.cpp src/net/udp_transport.cpp src/net/steam_transport.cpp)
target_include_directories(tldnet PUBLIC src)
target_link_libraries(tldnet PUBLIC ws2_32)

# The mod. The game loads it as winhttp.dll.
add_library(tldcoop SHARED
  src/dllmain.cpp
  src/avatar.cpp
  src/config.cpp
  src/coop_death.cpp
  src/coop_give.cpp
  src/coop_hud.cpp
  src/coop_lapse.cpp
  src/coop_sleep.cpp
  src/coop_menu.cpp
  src/coop_notices.cpp
  src/core.cpp
  src/dev_commands.cpp
  src/dumper.cpp
  src/game_state.cpp
  src/hooks.cpp
  src/il2cpp_api.cpp
  src/log.cpp
  src/net_runtime.cpp
  src/paths.cpp
  src/steam_dispatch.cpp
  src/test_harness.cpp
  src/ui_inspect.cpp
  src/wildlife_carcass.cpp
  src/wildlife_hits.cpp
  src/wildlife_sync.cpp
  src/wildlife_targets.cpp
  src/world_containers.cpp
  src/world_objects.cpp
  src/world_fires.cpp
  src/world_items.cpp
  src/world_join.cpp
  src/world_keep.cpp
  src/world_mode.cpp
  src/world_sync.cpp
)
set_target_properties(tldcoop PROPERTIES OUTPUT_NAME winhttp)
target_link_libraries(tldcoop PRIVATE minhook tldnet)

# Fake second player for solo testing.
add_executable(tldbot src/bot/main.cpp)
target_link_libraries(tldbot PRIVATE tldnet)

# Offline tests (no game needed).
add_executable(interp_test tests/interp_test.cpp)
target_link_libraries(interp_test PRIVATE tldnet)
add_executable(protocol_test tests/protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE tldnet)
add_executable(claims_test tests/claims_test.cpp)
target_link_libraries(claims_test PRIVATE tldnet)
add_executable(harvests_test tests/harvests_test.cpp)
target_link_libraries(harvests_test PRIVATE tldnet)
add_executable(session_test tests/session_test.cpp)
target_link_libraries(session_test PRIVATE tldnet)
add_executable(lapse_test tests/lapse_test.cpp)
target_link_libraries(lapse_test PRIVATE tldnet)
add_executable(uniques_test tests/uniques_test.cpp)
target_link_libraries(uniques_test PRIVATE tldnet)
add_executable(sleep_vote_test tests/sleep_vote_test.cpp)
target_link_libraries(sleep_vote_test PRIVATE tldnet)
add_executable(held_test tests/held_test.cpp)
target_link_libraries(held_test PRIVATE tldnet)
add_executable(held_pose_test tests/held_pose_test.cpp)
target_link_libraries(held_pose_test PRIVATE tldnet)
add_executable(hud_test tests/hud_test.cpp)
target_link_libraries(hud_test PRIVATE tldnet)
add_executable(handover_test tests/handover_test.cpp)
target_link_libraries(handover_test PRIVATE tldnet)
