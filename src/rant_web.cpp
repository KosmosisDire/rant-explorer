/* The web build's one RANT_IMPLEMENTATION anchor. A page opens no node to compile schemas in,
   so each text gets a registry here, since two nodes may define one type name differently. */
#define RANT_IMPLEMENTATION
#include <rant.hpp>

#include <cstdlib>

namespace {

void* heap(void*, void* ptr, size_t size)
{
    if (!size) {
        std::free(ptr);
        return nullptr;
    }
    return std::realloc(ptr, size);
}

} /* namespace */

/* Schema text compiled for good, NULL with the reason in why. */
extern "C" const RantSchema* rant_web_schema_compile(const char* text, const char** why)
{
    auto* registry = (i_RantRegistry*)std::calloc(1, sizeof(i_RantRegistry));
    if (!registry) return nullptr;
    i_rant_registry_init(registry, heap, nullptr);
    i_RantSchemaErr err{};
    const RantSchema* schema = i_rant_registry_compile(registry, text, &err);
    if (!schema && why) *why = err.why;
    return schema;
}

/* A schema's wire bytes parsed for good, NULL when they are refused. */
extern "C" const RantSchema* rant_web_schema_parse(const void* wire, size_t size)
{
    return i_rant_schema_parse(wire, size, heap, nullptr);
}
