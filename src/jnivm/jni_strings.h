#ifndef MOCKTAIL_JNIVM_JNI_STRINGS_H_
#define MOCKTAIL_JNIVM_JNI_STRINGS_H_

#include <jni.h>

#include <string>
#include <string_view>

namespace jnivm::internal {

jstring MakeString(const char* utf);
jstring MakeUtf16String(const jchar* utf16, jsize length);
std::string_view StringViewFromJString(jstring str);
std::string StringFromJString(jstring str);
jbyteArray JavaStringGetUtf8Bytes(jobject obj, jstring charset_name);
const char* StringChars(jstring str);
const jchar* StringUtf16Chars(jstring str);
jsize StringUtf16Length(jstring str);
jsize StringModifiedUtf8Length(jstring str);
void CopyStringRegion(jstring str, jsize start, jsize length, jchar* output);
void CopyStringModifiedUtf8Region(jstring str, jsize start, jsize length,
                                  char* output);

}  // namespace jnivm::internal
#endif
