# source files
SET(SRCS
rtsp.cpp
)

SET(HEADERS
)

SET(SRC_1
${SRCS}
rtsp.cpp
)

INCLUDE_DIRECTORIES(
${screamIncludes}
)

LINK_DIRECTORIES(
${screamLink}
)

ADD_EXECUTABLE(rtsp ${SRC_1} ${HEADERS})


TARGET_LINK_LIBRARIES (
rtsp
${screamLibs}
)
