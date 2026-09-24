#include "icons.hpp"

#include "assets.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>

#include <cstdlib>
#include <map>
#include <string>

namespace {

std::map<std::string, unsigned> codepoints;   /* icon name to its private use codepoint */

/* The file is one flat object of "name": number pairs, so a scan for each quoted key and
   the digits after it reads it without a JSON library. */
bool load_codepoints()
{
    std::string text;
    if (!asset_read("lucide-codepoints.json", text)) return false;

    size_t pos = 0;
    while ((pos = text.find('"', pos)) != std::string::npos) {
        const size_t end = text.find('"', pos + 1);
        if (end == std::string::npos) break;
        const std::string name = text.substr(pos + 1, end - pos - 1);
        pos = text.find_first_not_of(" \t\r\n:", end + 1);
        if (pos == std::string::npos) break;
        if (text[pos] >= '0' && text[pos] <= '9')
            codepoints[name] = (unsigned)std::strtoul(text.c_str() + pos, nullptr, 10);
    }
    return !codepoints.empty();
}

class IconElement : public Rml::Element {
public:
    using Rml::Element::Element;

    void OnAttributeChange(const Rml::ElementAttributes& changed) override
    {
        Rml::Element::OnAttributeChange(changed);
        if (changed.find("name") == changed.end()) return;
        const Rml::String name = GetAttribute<Rml::String>("name", "");
        const auto it = codepoints.find(name);
        if (it == codepoints.end()) {
            SetInnerRML("");
            if (!name.empty())
                Rml::Log::Message(Rml::Log::LT_WARNING, "no Lucide icon named %s", name.c_str());
            return;
        }
        SetInnerRML(Rml::StringUtilities::ToUTF8((Rml::Character)it->second));
    }
};

Rml::ElementInstancerGeneric<IconElement> instancer;

} /* namespace */

bool icons_init()
{
    bool ok = Rml::LoadFontFace("lucide.ttf", "lucide",
                                Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal);
    ok = load_codepoints() && ok;
    Rml::Factory::RegisterElementInstancer("icon", &instancer);
    return ok;
}
