# Ordinary image decoding/encoding is shared below the application and widget
# layers. Each consumer links this one implementation; none depends on UI merely
# to turn compressed image bytes into owning pixels.
add_library(maichat_image_codec STATIC
  ../Media/MaiImageDecode.h ../Media/MaiImageDecode.cpp
  ../Media/MaiImageEncode.h ../Media/MaiImageEncode.cpp)
set_target_properties(maichat_image_codec PROPERTIES AUTOMOC OFF AUTORCC OFF)
target_include_directories(maichat_image_codec PUBLIC ../Media)
if(APPLE AND TARGET maiagent_embedded_fftools)
  set(_maichat_image_ffmpeg_include_dir
    "${CMAKE_CURRENT_BINARY_DIR}/maiagent/Graphics/ffmpeg-runtime/install/include")
  target_include_directories(maichat_image_codec PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/maiagent/Graphics/ffmpeg-runtime/install/include")
  target_link_libraries(maichat_image_codec PRIVATE maiagent_embedded_fftools)
elseif(WIN32 AND TARGET maichat_ffmpeg_tools)
  set(_maichat_image_ffmpeg_include_dir "${MAICHAT_FFMPEG_BUILD_DIR}/install/include")
  target_include_directories(maichat_image_codec PRIVATE
    "${MAICHAT_FFMPEG_BUILD_DIR}/install/include")
  target_link_libraries(maichat_image_codec PRIVATE
    maichat_ffmpeg_avformat maichat_ffmpeg_avcodec
    maichat_ffmpeg_swscale maichat_ffmpeg_swresample maichat_ffmpeg_avutil
    ${MAICHAT_MEDIA_CODEC_ARCHIVES}
    psapi ole32 strmiids uuid oleaut32 shlwapi gdi32 vfw32 ws2_32 user32 bcrypt)
else()
  message(FATAL_ERROR "Desktop raster images require the native FFmpeg media backend")
endif()

# QString/QByteArray and owning QImage stay in this small presentation adapter;
# application, Agent and widget code all depend downward on the ordinary codec.
add_library(maichat_image_presentation STATIC
  src/ui/DesktopRasterImage.h src/ui/DesktopRasterImage.cpp
  src/ui/DesktopDocumentImage.h src/ui/DesktopDocumentImage.cpp)
set_target_properties(maichat_image_presentation PROPERTIES AUTOMOC OFF AUTORCC OFF)
target_include_directories(maichat_image_presentation PUBLIC src)
target_link_libraries(maichat_image_presentation PUBLIC
  Qt6::Core Qt6::Gui maichat_image_codec)
# Self-contained SVG document resources use the vector renderer; compressed
# user bitmaps continue through the ordinary native codec above.
target_link_libraries(maichat_image_presentation PRIVATE Qt6::Svg)
