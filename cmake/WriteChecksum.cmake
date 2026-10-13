if(NOT DEFINED INPUT_FILE OR NOT EXISTS "${INPUT_FILE}")
  message(FATAL_ERROR "INPUT_FILE does not exist: ${INPUT_FILE}")
endif()
if(NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "OUTPUT_FILE is required")
endif()

file(SHA256 "${INPUT_FILE}" file_hash)
file(WRITE "${OUTPUT_FILE}" "${file_hash}  MouNeru.exe\n")
