file(READ "${INPUT_SPV}" SPV_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])" "0x\\4\\3\\2\\1u, " SPV_WORDS "${SPV_HEX}")
file(WRITE "${OUTPUT_HEADER}" "// Auto-generated SPIR-V binary header\n#pragma once\n#include <cstdint>\n#include <cstddef>\n\ninline constexpr uint32_t ${ARRAY_NAME}[] = {\n    ${SPV_WORDS}\n};\ninline constexpr size_t ${ARRAY_NAME}SizeBytes = sizeof(${ARRAY_NAME});\n")
