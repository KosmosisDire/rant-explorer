#include "keyboard.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Input.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {

Rml::Context* g_context = nullptr;   /* the one context the page's input types into */

bool is_text(Rml::Element* element)
{
    if (!element) return false;
    if (element->GetTagName() != "input") return false;
    const Rml::String type = element->GetAttribute<Rml::String>("type", "text");
    return type == "text" || type == "password";
}

#ifdef __EMSCRIPTEN__
/* The hidden input is made once. Its keys stop at it, so SDL, which hears keys on the
   window, never types them twice. A tap after a focus the page refused still raises it,
   since a phone opens its keyboard only inside a gesture. */
EM_JS_DEPS(keyboard, "$stringToNewUTF8,$UTF8ToString");
EM_JS(void, keyboard_open, (const char* text, const char* mode), {
    let input = Module.keyboard;
    if (!input) {
        input = Module.keyboard = document.createElement("input");
        for (const [k, v] of [["autocomplete", "off"], ["autocapitalize", "off"], ["autocorrect", "off"], ["spellcheck", "false"]])
            input.setAttribute(k, v);
        input.style.cssText = "position:fixed;left:0;top:0;width:1px;height:1px;opacity:0;border:0;padding:0;font-size:16px;pointer-events:none";
        const stop = (e) => e.stopPropagation();
        input.addEventListener("keyup", stop);
        input.addEventListener("keypress", stop);
        input.addEventListener("keydown", (e) => {
            e.stopPropagation();
            if (e.key === "Enter") { e.preventDefault(); _keyboard_enter(); }
        });
        input.addEventListener("input", () => { const p = stringToNewUTF8(input.value); _keyboard_text(p); _free(p); });
        input.addEventListener("blur", () => { if (Module.keyboardOpen) { Module.keyboardOpen = false; _keyboard_closed(); } });
        Module.canvas.addEventListener("touchend", () => { if (Module.keyboardOpen && document.activeElement !== input) input.focus(); });
        document.body.appendChild(input);
    }
    input.inputMode = UTF8ToString(mode);
    input.value = UTF8ToString(text);
    Module.keyboardOpen = true;
    input.focus();
    input.setSelectionRange(input.value.length, input.value.length);
});
EM_JS(void, keyboard_close, (), {
    Module.keyboardOpen = false;
    if (Module.keyboard) Module.keyboard.blur();
});
#else
void keyboard_open(const char*, const char*) {}
void keyboard_close() {}
#endif

}

extern "C" {

/* The hidden input's whole text, after each change to it. Setting a value sends no change,
   so it is sent as typing would, for the bindings and editors that hear it. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void keyboard_text(const char* text)
{
    Rml::Element* field = g_context ? g_context->GetFocusElement() : nullptr;
    if (!is_text(field)) return;
    auto* input = static_cast<Rml::ElementFormControlInput*>(field);
    input->SetValue(text);
    const int end = (int)input->GetValue().size();
    input->SetSelectionRange(end, end);
    field->DispatchEvent(Rml::EventId::Change, { { "value", Rml::Variant(input->GetValue()) }, { "linebreak", Rml::Variant(false) } });
}

/* Enter, as the SDL backend hands it on: the key, then a line break. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void keyboard_enter()
{
    if (!g_context) return;
    g_context->ProcessKeyDown(Rml::Input::KI_RETURN, 0);
    g_context->ProcessTextInput('\n');
    g_context->ProcessKeyUp(Rml::Input::KI_RETURN, 0);
}

/* The keyboard went away, so the field it was typing into lets go too. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void keyboard_closed()
{
    Rml::Element* field = g_context ? g_context->GetFocusElement() : nullptr;
    if (is_text(field)) field->Blur();
}

}

Keyboard::Keyboard(Rml::Context* context, const bool& touch) : touch_(touch)
{
    g_context = context;
}

void Keyboard::listen(Rml::ElementDocument* document)
{
    document->AddEventListener(Rml::EventId::Focus, this, true);
    document->AddEventListener(Rml::EventId::Blur, this, true);
}

void Keyboard::ProcessEvent(Rml::Event& event)
{
    Rml::Element* field = event.GetTargetElement();
    if (!is_text(field)) return;
    if (event.GetId() == Rml::EventId::Blur) {
        keyboard_close();
        return;
    }
    if (!touch_) return;
    const Rml::String text = static_cast<Rml::ElementFormControlInput*>(field)->GetValue();
    const Rml::String mode = field->GetAttribute<Rml::String>("inputmode", "text");
    keyboard_open(text.c_str(), mode.c_str());
}
