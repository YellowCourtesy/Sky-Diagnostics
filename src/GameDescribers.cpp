#include "PCH.h"

#include "GameDescribers.h"
#include "crash/CrashHandler.h"

// These functions run after a crash, on the report thread. They must not
// allocate memory and must not take locks. The object can be damaged: the
// crash handler catches faults in these functions.

namespace skydiag
{
	namespace
	{
		void Append(char* a_out, std::size_t a_cap, const char* a_fmt, ...)
		{
			const std::size_t used = std::strlen(a_out);
			if (used + 1 >= a_cap) {
				return;
			}
			va_list args;
			va_start(args, a_fmt);
			std::vsnprintf(a_out + used, a_cap - used, a_fmt, args);
			va_end(args);
		}

		// Copies text and replaces characters that are not printable.
		void AppendText(char* a_out, std::size_t a_cap, const char* a_text, std::size_t a_max)
		{
			std::size_t used = std::strlen(a_out);
			for (std::size_t i = 0; i < a_max && a_text[i] != '\0' && used + 1 < a_cap; ++i) {
				const char c = a_text[i];
				a_out[used++] = (c >= 0x20 && c < 0x7F) ? c : '?';
			}
			a_out[used] = '\0';
		}

		const char* FileName(const RE::TESFile* a_file)
		{
			return a_file ? a_file->fileName : nullptr;
		}

		// Writes "Type FormID XXXXXXXX "name" (first file -> last file)".
		// Returns the file that changed the form last, or nullptr.
		const char* AppendForm(char* a_out, std::size_t a_cap, const RE::TESForm* a_form)
		{
			const auto type = RE::FormTypeToString(a_form->GetFormType());
			Append(a_out, a_cap, "%.*s FormID %08X", static_cast<int>(type.size()), type.data(), a_form->GetFormID());

			if (const char* name = a_form->GetName(); name && name[0] != '\0') {
				Append(a_out, a_cap, " \"");
				AppendText(a_out, a_cap, name, 60);
				Append(a_out, a_cap, "\"");
			}

			if (a_form->IsDynamicForm()) {
				Append(a_out, a_cap, " (made at run time, no plugin file)");
				return nullptr;
			}

			const char* first = FileName(a_form->GetFile(0));
			const char* last = FileName(a_form->GetFile(-1));
			if (first && last && first != last) {
				Append(a_out, a_cap, " (from ");
				AppendText(a_out, a_cap, first, 80);
				Append(a_out, a_cap, ", last changed by ");
				AppendText(a_out, a_cap, last, 80);
				Append(a_out, a_cap, ")");
			} else if (last) {
				Append(a_out, a_cap, " (from ");
				AppendText(a_out, a_cap, last, 80);
				Append(a_out, a_cap, ")");
			}
			return last;
		}

		void DescribeForm(void* a_subobject, char* a_text, std::size_t a_textCap, char* a_source, std::size_t a_sourceCap)
		{
			const auto  form = static_cast<const RE::TESForm*>(a_subobject);
			const char* source = AppendForm(a_text, a_textCap, form);

			// For a reference, the base object says what the thing is.
			const auto type = form->GetFormType();
			if (type == RE::FormType::Reference || type == RE::FormType::ActorCharacter) {
				const auto reference = static_cast<const RE::TESObjectREFR*>(form);
				if (const auto base = reference->GetBaseObject()) {
					Append(a_text, a_textCap, "; base object: ");
					const char* baseSource = AppendForm(a_text, a_textCap, base);
					if (!source) {
						source = baseSource;
					}
				}
			}

			if (source) {
				AppendText(a_source, a_sourceCap, source, a_sourceCap);
			}
		}

		// Scene graph objects and textures. The name of a texture object is
		// its file path. The name of a node is the node name in the mesh file.
		void DescribeNiObject(void* a_subobject, char* a_text, std::size_t a_textCap, char*, std::size_t)
		{
			const auto  object = static_cast<const RE::NiObjectNET*>(a_subobject);
			const char* name = object->name.c_str();
			if (name && name[0] != '\0') {
				Append(a_text, a_textCap, "name \"");
				AppendText(a_text, a_textCap, name, 200);
				Append(a_text, a_textCap, "\"");
			}
		}
	}

	void RegisterGameDescribers()
	{
		crash::RegisterDescriber("TESForm", &DescribeForm);
		crash::RegisterDescriber("NiObjectNET", &DescribeNiObject);
	}
}
