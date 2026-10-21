# Included by modules/CMakeLists.txt after the `modules` target exists. cpp-httplib (deps/httplib.h) talks HTTPS to
# the LLM provider through OpenSSL; src/CortexChatClient.cpp defines CPPHTTPLIB_OPENSSL_SUPPORT itself.
find_package(OpenSSL REQUIRED)
target_link_libraries(modules PRIVATE OpenSSL::SSL OpenSSL::Crypto)
