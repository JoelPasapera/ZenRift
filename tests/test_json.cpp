#include <string>

#include "json/lex.hpp"
#include "json/value.hpp"
#include "json/writer.hpp"
#include "framework.hpp"

using namespace zr;

TEST(json_dom_analiza_documento_completo) {
  json::Value v;
  json::ParseError err;
  REQUIRE(json::parse(R"({"a": 1, "b": [true, false, null], "c": {"d": "x\u00e9\ud83d\ude00"}, "e": -1.5e3})", v,
                      &err));
  REQUIRE(v.is_object());
  CHECK_EQ(v.get_number("a", 0), 1.0);
  const json::Value* b = v.get("b");
  REQUIRE(b && b->is_array());
  CHECK_EQ(b->items().size(), size_t{3});
  CHECK(b->items()[0].as_bool());
  CHECK(b->items()[2].is_null());
  const json::Value* c = v.get("c");
  REQUIRE(c);
  CHECK_EQ(c->get_string("d"), std::string_view("x\xc3\xa9\xf0\x9f\x98\x80"));
  CHECK_EQ(v.get_number("e", 0), -1500.0);
  CHECK_EQ(json::to_string(v),
           std::string(R"({"a":1,"b":[true,false,null],"c":{"d":"x)") + "\xc3\xa9\xf0\x9f\x98\x80" + R"("},"e":-1500})");
}

TEST(json_dom_rechaza_entradas_invalidas) {
  const char* bad[] = {
      "",            "{",          "{\"a\":}",       "[1,]",          "{\"a\" 1}", "01",       "1.",
      "-",           "1e",         "\"\\x\"",        "\"a\nb\"",      "tru",       "nul",      "{} x",
      "[1 2]",       "\"\\u12G4\"", "\"\xff\"",      "\"\xc0\xaf\"",  "\"\xed\xa0\x80\"",
  };
  for (const char* s : bad) {
    json::Value v;
    json::ParseError err;
    if (json::parse(s, v, &err)) zt::report_failure(__FILE__, __LINE__, std::string("aceptó: ") + s);
  }
  // Anidamiento excesivo.
  std::string deep(100, '[');
  deep += std::string(100, ']');
  json::Value v;
  CHECK(!json::parse(deep, v));
}

TEST(json_sustituto_suelto_se_reemplaza) {
  json::Value v;
  REQUIRE(json::parse("\"a\\ud800b\"", v));
  CHECK_EQ(v.as_string(), std::string("a\xef\xbf\xbd" "b"));
}

TEST(json_numeros) {
  const char* p;
  double d;
  const std::string a = "123.25e-2,";
  p = a.data();
  REQUIRE(json::lex::scan_number(p, a.data() + a.size(), d));
  CHECK_NEAR(d, 1.2325, 1e-15);
  CHECK_EQ(*p, ',');
  const std::string b = "-0";
  p = b.data();
  REQUIRE(json::lex::scan_number(p, b.data() + b.size(), d));
  CHECK_EQ(d, 0.0);
}

TEST(json_writer_compacto_y_con_comas) {
  std::string out;
  json::Writer w(out);
  w.begin_object().kv("nombre", "zén\"rift").kv_num("v", 2.5).key("lista").begin_array().integer(1).null().boolean(
      false).begin_object().end_object().end_array().key("vacio").begin_array().end_array().end_object();
  CHECK_EQ(out, std::string(R"({"nombre":"zén\"rift","v":2.5,"lista":[1,null,false,{}],"vacio":[]})"));
  json::Value v;
  CHECK(json::parse(out, v));
}

TEST(json_skip_value_salta_estructuras) {
  const std::string s = R"({"a":[1,{"b":"}"}],"c":"d"} tail)";
  const char* p = s.data();
  REQUIRE(json::lex::skip_value(p, s.data() + s.size()));
  CHECK_EQ(std::string_view(p), std::string_view(" tail"));
}
