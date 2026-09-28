#pragma once
#include <windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <string>

// Both DirectWrite and the native edit control use the same private, embedded font.
// Nothing is installed in Windows or read beside the executable at runtime.
class EmbeddedFont {
    Microsoft::WRL::ComPtr<IDWriteFactory5> factory;
    Microsoft::WRL::ComPtr<IDWriteInMemoryFontFileLoader> loader;
    Microsoft::WRL::ComPtr<IDWriteFontCollection1> fonts;
    HANDLE gdiFont = nullptr;
    bool registered = false;

  public:
    std::wstring family;
    ~EmbeddedFont() {
        fonts.Reset();
        if (registered)
            factory->UnregisterFontFileLoader(loader.Get());
        loader.Reset();
        if (gdiFont)
            RemoveFontMemResourceEx(gdiFont);
    }
    IDWriteFontCollection *collection() const {
        return fonts.Get();
    }
    bool load(HINSTANCE module, IDWriteFactory *baseFactory) {
        auto resource = FindResourceW(module, MAKEINTRESOURCEW(102), RT_RCDATA);
        if (!resource || !baseFactory)
            return false;
        auto data = LockResource(LoadResource(module, resource));
        DWORD bytes = SizeofResource(module, resource), fontCount = 0;
        if (!data || !bytes)
            return false;
        gdiFont = AddFontMemResourceEx(data, bytes, nullptr, &fontCount);
        if (!gdiFont || !fontCount || FAILED(baseFactory->QueryInterface(IID_PPV_ARGS(&factory))))
            return false;
        if (FAILED(factory->CreateInMemoryFontFileLoader(&loader)) ||
            FAILED(factory->RegisterFontFileLoader(loader.Get())))
            return false;
        registered = true;
        Microsoft::WRL::ComPtr<IDWriteFontFile> file;
        Microsoft::WRL::ComPtr<IDWriteFontSetBuilder1> builder;
        Microsoft::WRL::ComPtr<IDWriteFontSet> set;
        if (FAILED(loader->CreateInMemoryFontFileReference(factory.Get(), data, bytes, nullptr, &file)) ||
            FAILED(factory->CreateFontSetBuilder(&builder)) || FAILED(builder->AddFontFile(file.Get())) ||
            FAILED(builder->CreateFontSet(&set)) ||
            FAILED(factory->CreateFontCollectionFromFontSet(set.Get(), &fonts)))
            return false;
        Microsoft::WRL::ComPtr<IDWriteFontFamily> fontFamily;
        Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(fonts->GetFontFamily(0, &fontFamily)) || FAILED(fontFamily->GetFamilyNames(&names)))
            return false;
        UINT32 index = 0, length = 0;
        BOOL exists = FALSE;
        names->FindLocaleName(L"en-US", &index, &exists);
        if (!exists)
            index = 0;
        if (FAILED(names->GetStringLength(index, &length)))
            return false;
        family.resize(length + 1);
        if (FAILED(names->GetString(index, family.data(), length + 1)))
            return false;
        family.resize(length);
        return !family.empty();
    }
};
