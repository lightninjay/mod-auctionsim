set(AUCTIONSIM_DAT "${CMAKE_CURRENT_SOURCE_DIR}/mod-auctionsim/data/auctionsim.dat")

if(WIN32)
  add_custom_command(TARGET modules POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_FILE_DIR:worldserver>/configs/modules"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${AUCTIONSIM_DAT}"
      "$<TARGET_FILE_DIR:worldserver>/configs/modules/auctionsim.dat"
    COMMENT "Copy auctionsim.dat into configs/modules")
  install(FILES "${AUCTIONSIM_DAT}" DESTINATION "${CMAKE_INSTALL_PREFIX}/configs/modules")
else()
  install(FILES "${AUCTIONSIM_DAT}" DESTINATION "${CONF_DIR}/modules")
endif()

add_compile_options(-Wall -Wextra -Werror)
