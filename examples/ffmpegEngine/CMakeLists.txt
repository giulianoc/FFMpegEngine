
# Copyright (C) Giuliano Catrambone (giulianocatrambone@gmail.com)

# This program is free software; you can redistribute it and/or 
# modify it under the terms of the GNU General Public License 
# as published by the Free Software Foundation; either 
# version 2 of the License, or (at your option) any later 
# version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

# Commercial use other than under the terms of the GNU General Public
# License is allowed only after express negotiation of conditions
# with the authors.

SET (SOURCES
        ffmpegEngine.cpp
)

SET (HEADERS
)

#include_directories("${NLOHMANN_INCLUDE_DIR}")
#include_directories("${SPDLOG_INCLUDE_DIR}")
#include_directories("${FFMPEGENGINE_INCLUDE_DIR}")
link_directories("${STRINGUTILS_LIB_DIR}")

add_executable(ffmpegEngine ${SOURCES} ${HEADERS})

target_link_libraries (ffmpegEngine StringUtils)
target_link_libraries (ffmpegEngine FFMpegEngine)

