#pragma once

// A small non-validating XML reader, enough for trophy SFM files: elements,
// attributes, text, CDATA, comments, processing instructions, DOCTYPE and the
// predefined/numeric entities. No namespaces or DTD entities.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace recomp::achievements::detail {

struct XmlNode {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::string text;  // Concatenated character data directly inside this element.
  std::vector<XmlNode> children;

  const XmlNode* child(std::string_view child_name) const;
  // Attribute value, or fallback when absent.
  std::string attr(std::string_view attr_name, std::string_view fallback = {}) const;
  bool has_attr(std::string_view attr_name) const;
  // text with surrounding whitespace removed.
  std::string trimmed_text() const;
  // trimmed_text() of the named child, or empty.
  std::string child_text(std::string_view child_name) const;
};

// Parses a document and stores its root element in *root.
bool ParseXml(std::string_view xml, XmlNode* root, std::string* error = nullptr);

}  // namespace recomp::achievements::detail
