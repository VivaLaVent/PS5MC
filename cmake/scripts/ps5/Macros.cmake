# Kodi normally builds libdvdnav into a dlopen()-ed wrapper .so under
# system/players/VideoPlayer. Homebrew has no disc access and the wrapper
# depends on the linux dll-loader export trick, so skip it (as the wasm port
# does). DVD playback is out of scope for the console.
function(core_link_library lib wraplib)
  message(STATUS "PS5: skipping core_link_library(${lib} ${wraplib}) - no dlopen wrapper for DVD libs")
endfunction()
