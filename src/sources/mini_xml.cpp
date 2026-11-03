#include "mini_xml.h"

#include <cstdint>

namespace recomp::achievements::detail {
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool IsNameChar(char c) {
  return !IsSpace(c) && c != '>' && c != '/' && c != '=' && c != '<' && c != '"' && c != '\'' &&
         c != '\0';
}

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp <= 0x10FFFF) {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// Unknown or malformed entities are kept literally: trophy text written by
// hand sometimes has a bare '&'.
void AppendDecoded(std::string& out, std::string_view s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    const size_t semi = s.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 12) {
      out += '&';
      continue;
    }
    std::string_view ent = s.substr(i + 1, semi - i - 1);
    bool ok = true;
    if (ent == "amp") out += '&';
    else if (ent == "lt") out += '<';
    else if (ent == "gt") out += '>';
    else if (ent == "quot") out += '"';
    else if (ent == "apos") out += '\'';
    else if (ent.size() > 1 && ent[0] == '#') {
      uint32_t cp = 0;
      const bool hex = ent[1] == 'x' || ent[1] == 'X';
      std::string_view digits = ent.substr(hex ? 2 : 1);
      ok = !digits.empty();
      for (char c : digits) {
        uint32_t d;
        if (c >= '0' && c <= '9') d = uint32_t(c - '0');
        else if (hex && c >= 'a' && c <= 'f') d = uint32_t(c - 'a' + 10);
        else if (hex && c >= 'A' && c <= 'F') d = uint32_t(c - 'A' + 10);
        else { ok = false; break; }
        cp = cp * (hex ? 16 : 10) + d;
        if (cp > 0x10FFFF) { ok = false; break; }
      }
      if (ok) AppendUtf8(out, cp);
    } else {
      ok = false;
    }
    if (ok) {
      i = semi;
    } else {
      out += '&';
    }
  }
}

class Parser {
 public:
  explicit Parser(std::string_view s) : s_(s) {}

  bool ParseDocument(XmlNode* root, std::string* error) {
    if (s_.starts_with("\xEF\xBB\xBF")) pos_ = 3;
    for (;;) {
      SkipSpace();
      if (pos_ >= s_.size()) return Fail(error, "no root element");
      if (!SkipMisc()) {
        if (s_[pos_] != '<') return Fail(error, "text outside the root element");
        break;
      }
      if (failed_) return Fail(error, failure_);
    }
    if (!ParseElement(root, 0)) return Fail(error, failure_);
    return true;
  }

 private:
  bool Fail(std::string* error, std::string message) {
    if (error) *error = "XML: " + message + " (at byte " + std::to_string(pos_) + ")";
    return false;
  }
  bool SetFailure(std::string message) {
    failed_ = true;
    failure_ = std::move(message);
    return false;
  }

  void SkipSpace() {
    while (pos_ < s_.size() && IsSpace(s_[pos_])) ++pos_;
  }

  bool SkipPast(std::string_view terminator) {
    const size_t end = s_.find(terminator, pos_);
    if (end == std::string_view::npos) {
      pos_ = s_.size();
      return SetFailure("unterminated markup");
    }
    pos_ = end + terminator.size();
    return true;
  }

  // Skips one comment, PI or DOCTYPE at pos_. Returns false if none is there.
  bool SkipMisc() {
    std::string_view rest = s_.substr(pos_);
    if (rest.starts_with("<!--")) return SkipPast("-->"), true;
    if (rest.starts_with("<?")) return SkipPast("?>"), true;
    if (rest.starts_with("<!DOCTYPE") || rest.starts_with("<!doctype")) {
      // Internal subsets ([...]) are skipped by bracket depth.
      int depth = 0;
      for (; pos_ < s_.size(); ++pos_) {
        if (s_[pos_] == '[') ++depth;
        else if (s_[pos_] == ']') --depth;
        else if (s_[pos_] == '>' && depth <= 0) {
          ++pos_;
          return true;
        }
      }
      SetFailure("unterminated DOCTYPE");
      return true;
    }
    return false;
  }

  std::string ParseName() {
    const size_t start = pos_;
    while (pos_ < s_.size() && IsNameChar(s_[pos_])) ++pos_;
    return std::string(s_.substr(start, pos_ - start));
  }

  bool ParseElement(XmlNode* node, int depth) {
    if (depth > 256) return SetFailure("elements nested too deeply");
    ++pos_;  // '<'
    node->name = ParseName();
    if (node->name.empty()) return SetFailure("missing element name");
    for (;;) {
      SkipSpace();
      if (pos_ >= s_.size()) return SetFailure("unterminated start tag <" + node->name + ">");
      if (s_[pos_] == '/') {
        if (pos_ + 1 >= s_.size() || s_[pos_ + 1] != '>') return SetFailure("bad empty-element tag");
        pos_ += 2;
        return true;
      }
      if (s_[pos_] == '>') {
        ++pos_;
        break;
      }
      std::string attr = ParseName();
      if (attr.empty()) return SetFailure("bad attribute in <" + node->name + ">");
      SkipSpace();
      std::string value;
      if (pos_ < s_.size() && s_[pos_] == '=') {
        ++pos_;
        SkipSpace();
        if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\'')) {
          return SetFailure("unquoted attribute value");
        }
        const char quote = s_[pos_++];
        const size_t end = s_.find(quote, pos_);
        if (end == std::string_view::npos) return SetFailure("unterminated attribute value");
        AppendDecoded(value, s_.substr(pos_, end - pos_));
        pos_ = end + 1;
      }
      node->attributes.emplace_back(std::move(attr), std::move(value));
    }

    // Content.
    for (;;) {
      const size_t lt = s_.find('<', pos_);
      if (lt == std::string_view::npos) {
        pos_ = s_.size();
        return SetFailure("missing </" + node->name + ">");
      }
      AppendDecoded(node->text, s_.substr(pos_, lt - pos_));
      pos_ = lt;
      std::string_view rest = s_.substr(pos_);
      if (rest.starts_with("</")) {
        pos_ += 2;
        std::string closing = ParseName();
        SkipSpace();
        if (pos_ >= s_.size() || s_[pos_] != '>') return SetFailure("bad end tag");
        ++pos_;
        if (closing != node->name) {
          return SetFailure("</" + closing + "> closes <" + node->name + ">");
        }
        return true;
      }
      if (rest.starts_with("<![CDATA[")) {
        pos_ += 9;
        const size_t end = s_.find("]]>", pos_);
        if (end == std::string_view::npos) return SetFailure("unterminated CDATA");
        node->text.append(s_.substr(pos_, end - pos_));
        pos_ = end + 3;
        continue;
      }
      if (SkipMisc()) {
        if (failed_) return false;
        continue;
      }
      node->children.emplace_back();
      if (!ParseElement(&node->children.back(), depth + 1)) return false;
    }
  }

  std::string_view s_;
  size_t pos_ = 0;
  bool failed_ = false;
  std::string failure_;
};

}  // namespace

const XmlNode* XmlNode::child(std::string_view child_name) const {
  for (const XmlNode& c : children) {
    if (c.name == child_name) return &c;
  }
  return nullptr;
}

std::string XmlNode::attr(std::string_view attr_name, std::string_view fallback) const {
  for (const auto& [k, v] : attributes) {
    if (k == attr_name) return v;
  }
  return std::string(fallback);
}

bool XmlNode::has_attr(std::string_view attr_name) const {
  for (const auto& kv : attributes) {
    if (kv.first == attr_name) return true;
  }
  return false;
}

std::string XmlNode::trimmed_text() const {
  size_t b = 0, e = text.size();
  while (b < e && IsSpace(text[b])) ++b;
  while (e > b && IsSpace(text[e - 1])) --e;
  return text.substr(b, e - b);
}

std::string XmlNode::child_text(std::string_view child_name) const {
  const XmlNode* c = child(child_name);
  return c ? c->trimmed_text() : std::string();
}

bool ParseXml(std::string_view xml, XmlNode* root, std::string* error) {
  *root = XmlNode();
  return Parser(xml).ParseDocument(root, error);
}

}  // namespace recomp::achievements::detail
