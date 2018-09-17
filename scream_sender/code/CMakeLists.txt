# source files
SET(SRCS
RtpQueue.cpp
ScreamTx.cpp
scream_sender.cpp
)

SET(HEADERS
RtpQueue.h
ScreamRx.h
ScreamTx.h
)

SET(SRC_1
${SRCS}
scream_sender.cpp
)

INCLUDE_DIRECTORIES(
${screamIncludes}
)

LINK_DIRECTORIES(
${screamLink}
)

ADD_EXECUTABLE(scream_sender ${SRC_1} ${HEADERS})


TARGET_LINK_LIBRARIES (
scream_sender
${screamLibs}
)
