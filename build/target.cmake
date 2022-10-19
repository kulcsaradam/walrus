# default set of each flag
SET (WALRUS_CXXFLAGS -fno-rtti) # build walrus without rtti
SET (WALRUS_CXXFLAGS_DEBUG)
SET (WALRUS_CXXFLAGS_RELEASE)
SET (WALRUS_LDFLAGS)
SET (WALRUS_DEFINITIONS)
SET (WALRUS_THIRDPARTY_CFLAGS)

SET (WALRUS_BUILD_32BIT OFF)

IF (${WALRUS_HOST} STREQUAL "linux")
    # default set of LDFLAGS
    SET (WALRUS_LDFLAGS -lpthread -lrt -Wl,--gc-sections)
    IF ((${WALRUS_ARCH} STREQUAL "x64") OR (${WALRUS_ARCH} STREQUAL "x86_64"))
    ELSEIF ((${WALRUS_ARCH} STREQUAL "x86") OR (${WALRUS_ARCH} STREQUAL "i686"))
        SET (WALRUS_BUILD_32BIT ON)
        SET (WALRUS_CXXFLAGS ${WALRUS_CXXFLAGS} -m32 -mfpmath=sse -msse -msse2)                                                                                                                              
        SET (WALRUS_LDFLAGS ${WALRUS_LDFLAGS} -m32)
        SET (WALRUS_THIRDPARTY_CFLAGS -m32)
    ELSEIF (${WALRUS_ARCH} STREQUAL "arm")
        SET (WALRUS_BUILD_32BIT ON)
        SET (WALRUS_CXXFLAGS ${WALRUS_CXXFLAGS} -march=armv7-a -mthumb)
    ELSEIF (${WALRUS_ARCH} STREQUAL "aarch64")
    ELSE()
        MESSAGE (FATAL_ERROR ${WALRUS_ARCH} " is unsupported")
    ENDIF()
ELSEIF (${WALRUS_HOST} STREQUAL "tizen_obs")
    # default set of LDFLAGS
    SET (WALRUS_LDFLAGS -lpthread -lrt -Wl,--gc-sections)
    SET (WALRUS_DEFINITIONS -DWALRUS_TIZEN)
    IF ((${WALRUS_ARCH} STREQUAL "x64") OR (${WALRUS_ARCH} STREQUAL "x86_64"))
    ELSEIF ((${WALRUS_ARCH} STREQUAL "x86") OR (${WALRUS_ARCH} STREQUAL "i686"))
        SET (WALRUS_BUILD_32BIT ON)
        SET (WALRUS_CXXFLAGS ${WALRUS_CXXFLAGS} -m32 -mfpmath=sse -msse -msse2)
        SET (WALRUS_LDFLAGS ${WALRUS_LDFLAGS} -m32)
        SET (WALRUS_THIRDPARTY_CFLAGS -m32)
    ELSEIF (${WALRUS_ARCH} STREQUAL "arm")
        SET (WALRUS_BUILD_32BIT ON)
        SET (WALRUS_CXXFLAGS_DEBUG -O1)
        SET (WALRUS_CXXFLAGS_RELEASE -O2)
    ELSEIF (${WALRUS_ARCH} STREQUAL "aarch64")
    ELSE()
        MESSAGE (FATAL_ERROR ${WALRUS_ARCH} " is unsupported")
    ENDIF()
ELSEIF (${WALRUS_HOST} STREQUAL "android" AND ${WALRUS_ARCH} STREQUAL "arm")
    SET (WALRUS_BUILD_32BIT ON)
    SET (WALRUS_LDFLAGS -fPIE -pie -march=armv7-a -Wl,--fix-cortex-a8 -llog -Wl,--gc-sections)
    SET (WALRUS_DEFINITIONS -DANDROID=1)
ELSEIF (${WALRUS_HOST} STREQUAL "darwin" AND ${WALRUS_ARCH} STREQUAL "x64")
    SET (WALRUS_LDFLAGS -lpthread -Wl,-dead_strip)
ELSE()
    MESSAGE (FATAL_ERROR ${WALRUS_HOST} " with " ${WALRUS_ARCH} " is unsupported")
ENDIF()

IF (WALRUS_BUILD_32BIT)
    # 32bit build
    SET (WALRUS_DEFINITIONS ${WALRUS_DEFINITIONS} -DWALRUS_32=1)
ELSE()
    # 64bit build
    SET (WALRUS_DEFINITIONS ${WALRUS_DEFINITIONS} -DWALRUS_64=1)
    SET (WALRUS_THIRDPARTY_CFLAGS ${WALRUS_THIRDPARTY_CFLAGS})
ENDIF()