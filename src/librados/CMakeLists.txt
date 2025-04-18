add_library(librados_impl STATIC
  IoCtxImpl.cc
  RadosXattrIter.cc
  RadosClient.cc
  librados_util.cc
  librados_tp.cc)
target_link_libraries(librados_impl
  PRIVATE legacy-option-headers)
# C/C++ API
add_library(librados ${CEPH_SHARED}
  librados_c.cc
  librados_cxx.cc
  $<TARGET_OBJECTS:common_buffer_obj>)
if(ENABLE_SHARED)
  set_target_properties(librados PROPERTIES
    OUTPUT_NAME rados
    VERSION 2.0.0
    SOVERSION 2
    VISIBILITY_INLINES_HIDDEN ON)
  if(HAVE_LINK_EXCLUDE_LIBS AND NOT WIN32)
    set_property(TARGET librados APPEND_STRING PROPERTY
      LINK_FLAGS " -Wl,--exclude-libs,ALL")
  endif()
  if(HAVE_LINK_VERSION_SCRIPT AND NOT WIN32)
    set_property(TARGET librados APPEND_STRING PROPERTY
      LINK_FLAGS " -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/librados.map")
  endif()
  if(WITH_STATIC_LIBSTDCXX)
    set_property(TARGET librados APPEND_STRING PROPERTY
      LINK_FLAGS " -static-libstdc++ -static-libgcc")
  endif()
  target_compile_definitions(librados PRIVATE -DLIBRADOS_SHARED=1)
endif()
target_link_libraries(librados PRIVATE
  librados_impl osdc ceph-common cls_lock_client
  ${BLKID_LIBRARIES} ${CRYPTO_LIBS} ${EXTRALIBS} ${GSSAPI_LIBRARIES})
install(TARGETS librados DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(WITH_LTTNG)
  add_dependencies(librados_impl librados-tp)
  if(WITH_EVENTTRACE)
    add_dependencies(librados_impl eventtrace_tp)
  endif()
endif()
