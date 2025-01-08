//
// lpg: Lua PDF generator
//
// Copyright (c) 2017 - 2025, Přemysl Eric Janouch <p@janouch.name>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
// OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <cairo.h>
#include <cairo-pdf.h>
#include <pango/pangocairo.h>

#include <qrencode.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

#include <arpa/inet.h>

using namespace std;
using attribute = variant<string, double>;

#define DefWidget(name) struct name : public Widget
struct Widget {
	virtual ~Widget() {}

	unordered_map<string, attribute> attributes;
	using attribute_map = decltype(attributes);
	Widget *setattr(string key, attribute_map::mapped_type value) {
		attributes.insert({key, value});
		return this;
	}

	optional<attribute> getattr(const string &name) {
		if (auto it = attributes.find("_" + name); it != attributes.end())
			return {it->second};
		if (auto it = attributes.find(name); it != attributes.end())
			return {it->second};
		return {};
	}

	/// Top-down attribute propagation.
	virtual void apply_attributes(const attribute_map &attrs = {}) {
		for (const auto &kv : attrs)
			if (*kv.first.c_str() != '_')
				attributes.insert(kv);
	}

	// We need CAIRO_ROUND_GLYPH_POS_OFF to be set in font options,
	// which can only be done internally, se we have to pass a context that
	// is based on an actual PDF surface.
	//
	// Font maps also need some kind of a backend, like Cairo.

	/// Compute and return space required for the widget's contents.
	virtual tuple<double, double> prepare([[maybe_unused]] PangoContext *pc) {
		return {0, 0};
	}

	/// Compute and return space required for the widget's contents,
	/// given a fixed size (favouring any dimension).
	virtual tuple<double, double> prepare_for_size(PangoContext *pc,
		[[maybe_unused]] double width, [[maybe_unused]] double height) {
		return prepare(pc);
	}

	/// Render to the context within the designated space, no clipping.
	virtual void render([[maybe_unused]] cairo_t *cr, [[maybe_unused]] double w,
		[[maybe_unused]] double h) {}
};

/// Special container that basically just fucks with the system right now.
DefWidget(Frame) {
	unique_ptr<Widget> child;
	Frame(Widget *w) : child(w) {}

	virtual void apply_attributes(const attribute_map &attrs) override {
		Widget::apply_attributes(attrs);
		child->apply_attributes(attributes);
	}

	virtual tuple<double, double> prepare(PangoContext *pc) override {
		auto d = child->prepare(pc);
		if (auto v = getattr("w_override"))
			get<0>(d) = get<double>(*v);
		if (auto v = getattr("h_override"))
			get<1>(d) = get<double>(*v);
		return d;
	}

	virtual void render(cairo_t *cr, double w, double h) override {
		cairo_save(cr);

		if (auto v = getattr("color")) {
			int rgb = get<double>(*v);
			cairo_set_source_rgb(cr, ((rgb >> 16) & 0xFF) / 255.,
				((rgb >> 8) & 0xFF) / 255., (rgb & 0xFF) / 255.);
		}

		child->render(cr, w, h);
		cairo_restore(cr);
	}
};

#define DefContainer(name) struct name : public Container
DefWidget(Container) {
	vector<unique_ptr<Widget>> children;

	inline void add() {}
	template <typename... Args> void add(Widget *w, Args && ...args) {
		children.push_back(unique_ptr<Widget>(w));
		add(args...);
	}

	Container(vector<unique_ptr<Widget>> &&children)
		: children(std::move(children)) {}

	virtual void apply_attributes(const attribute_map &attrs) override {
		Widget::apply_attributes(attrs);
		for (auto &i : children)
			i->apply_attributes(attributes);
	}
};

static void finalize_box(vector<double> &sizes, double available) {
	double fixed = 0, stretched = 0;
	for (auto s : sizes) {
		if (s >= 0)
			fixed += s;
		else
			stretched += s;
	}
	if (stretched) {
		auto factor = max(0., available - fixed) / stretched;
		for (auto &s : sizes)
			if (s < 0)
				s *= factor;
	} else {
		// TODO(p): One should be able to *opt in* for this.
		auto redistribute = max(0., available - fixed) / sizes.size();
		for (auto &s : sizes)
			s += redistribute;
	}
}

DefContainer(HBox) {
	HBox(vector<unique_ptr<Widget>> children = {})
		: Container(std::move(children)) {}

	vector<double> widths;
	virtual tuple<double, double> prepare(PangoContext *pc) override {
		double w = 0, h = 0;
		widths.resize(children.size());
		for (size_t i = 0; i < children.size(); i++) {
			auto d = children[i]->prepare(pc);
			if ((widths[i] = get<0>(d)) > 0)
				w += widths[i];
			h = max(h, get<1>(d));
		}
		return {w, h};
	}

	virtual tuple<double, double> prepare_for_size(
		PangoContext *pc, double width, double height) override {
		double w = 0, h = 0;
		widths.resize(children.size());
		for (size_t i = 0; i < children.size(); i++) {
			auto d = children[i]->prepare_for_size(pc, width, height);
			if ((widths[i] = get<0>(d)) > 0)
				w += widths[i];
			h = max(h, get<1>(d));
		}
		return {w, h};
	}

	virtual void render(cairo_t *cr, double w, double h) override {
		finalize_box(widths, w);
		for (size_t i = 0; i < children.size(); i++) {
			cairo_save(cr);
			children[i]->render(cr, widths[i], h);
			cairo_restore(cr);
			cairo_translate(cr, widths[i], 0.);
		}
	}
};

DefContainer(VBox) {
	VBox(vector<unique_ptr<Widget>> children = {})
		: Container(std::move(children)) {}

	vector<double> heights;
	virtual tuple<double, double> prepare(PangoContext *pc) override {
		double w = 0, h = 0;
		heights.resize(children.size());
		for (size_t i = 0; i < children.size(); i++) {
			auto d = children[i]->prepare(pc);
			if ((heights[i] = get<1>(d)) > 0)
				h += heights[i];
			w = max(w, get<0>(d));
		}
		return {w, h};
	}

	virtual tuple<double, double> prepare_for_size(
		PangoContext *pc, double width, double height) override {
		double w = 0, h = 0;
		heights.resize(children.size());
		for (size_t i = 0; i < children.size(); i++) {
			auto d = children[i]->prepare_for_size(pc, width, height);
			if ((heights[i] = get<1>(d)) > 0)
				h += heights[i];
			w = max(w, get<0>(d));
		}
		return {w, h};
	}

	virtual void render(cairo_t *cr, double w, double h) override {
		finalize_box(heights, h);
		for (size_t i = 0; i < children.size(); i++) {
			cairo_save(cr);
			children[i]->render(cr, w, heights[i]);
			cairo_restore(cr);
			cairo_translate(cr, 0., heights[i]);
		}
	}
};

/// Fillers just take up space and don't render anything.
DefWidget(Filler) {
	double w, h;
	Filler(double w = -1, double h = -1) : w(w), h(h) {}
	virtual tuple<double, double> prepare(
		[[maybe_unused]] PangoContext *pc) override {
		return {w, h};
	}
};

DefWidget(HLine) {
	double thickness;
	HLine(double thickness = 1) : thickness(thickness) {}
	virtual tuple<double, double> prepare(
		[[maybe_unused]] PangoContext *pc) override {
		return {-1, thickness};
	}
	virtual void render(cairo_t *cr, double w, double h) override {
		cairo_move_to(cr, 0, h / 2);
		cairo_line_to(cr, w, h / 2);
		cairo_set_line_width(cr, thickness);
		cairo_stroke(cr);
	}
};

DefWidget(VLine) {
	double thickness;
	VLine(double thickness = 1) : thickness(thickness) {}
	virtual tuple<double, double> prepare(
		[[maybe_unused]] PangoContext *pc) override {
		return {thickness, -1};
	}
	virtual void render(cairo_t *cr, double w, double h) override {
		cairo_move_to(cr, w / 2, 0);
		cairo_line_to(cr, w / 2, h);
		cairo_set_line_width(cr, thickness);
		cairo_stroke(cr);
	}
};

DefWidget(Text) {
	string text;
	PangoLayout *layout = nullptr;
	double y_offset = 0.;

	Text(string text = "") : text(text) {}
	virtual ~Text() override { g_clear_object(&layout); }

	static string escape(const char *s, size_t len) {
		auto escapechar = [](char c) -> const char * {
			if (c == '<') return "&lt;";
			if (c == '>') return "&gt;";
			if (c == '&') return "&amp;";
			return nullptr;
		};
		string escaped;
		for (size_t i = 0; i < len; i++)
			if (auto entity = escapechar(s[i]))
				escaped += entity;
			else
				escaped += s[i];
		return escaped;
	}

	void prepare_layout(PangoContext *pc) {
		g_clear_object(&layout);
		layout = pango_layout_new(pc);
		pango_layout_set_markup(layout, text.c_str(), -1);
		pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);

		auto fd = pango_font_description_new();
		if (auto v = getattr("fontfamily"))
			pango_font_description_set_family(fd, get<string>(*v).c_str());
		if (auto v = getattr("fontsize"))
			pango_font_description_set_size(fd, get<double>(*v) * PANGO_SCALE);
		if (auto v = getattr("fontweight"))
			pango_font_description_set_weight(fd, PangoWeight(get<double>(*v)));

		// We need this for the line-height calculation.
		auto font_size =
			double(pango_font_description_get_size(fd)) / PANGO_SCALE;
		if (!font_size)
			pango_font_description_set_size(fd, (font_size = 10));

		// Supposedly this is how this shit works.
		// XXX: This will never work if the markup changes the font size.
		if (auto v = getattr("lineheight")) {
			auto increment = get<double>(*v) - 1;
			y_offset = increment * font_size / 2;
			pango_layout_set_spacing(
				layout, increment * font_size * PANGO_SCALE);
		}

		// FIXME: We don't want to override what's in the markup.
		pango_layout_set_font_description(layout, fd);
		pango_font_description_free(fd);
	}

	virtual tuple<double, double> prepare(PangoContext *pc) override {
		prepare_layout(pc);

		int w, h;
		pango_layout_get_size(layout, &w, &h);
		return {
			double(w) / PANGO_SCALE, double(h) / PANGO_SCALE + 2 * y_offset};
	}

	virtual tuple<double, double> prepare_for_size(PangoContext *pc,
		double width, [[maybe_unused]] double height) override {
		prepare_layout(pc);

		// It's difficult to get vertical text, so wrap horizontally.
		pango_layout_set_width(layout, PANGO_SCALE * width);

		int w, h;
		pango_layout_get_size(layout, &w, &h);
		return {
			double(w) / PANGO_SCALE, double(h) / PANGO_SCALE + 2 * y_offset};
	}

	virtual void render(cairo_t *cr, double w, [[maybe_unused]] double h)
		override {
		g_return_if_fail(layout);
		// Assuming horizontal text, make it span the whole allocation.
		pango_layout_set_width(layout, PANGO_SCALE * w);
		pango_cairo_update_layout(cr, layout);
		cairo_translate(cr, 0, y_offset);
		pango_cairo_show_layout(cr, layout);
	}
};

DefWidget(Link) {
	string target_uri;
	unique_ptr<Widget> child;

	Link(const string &target_uri, Widget *w)
		: target_uri(target_uri), child(w) {}

	virtual void apply_attributes(const attribute_map &attrs) override {
		Widget::apply_attributes(attrs);
		child->apply_attributes(attributes);
	}

	virtual tuple<double, double> prepare(PangoContext *pc) override {
		return child->prepare(pc);
	}

	virtual void render(cairo_t *cr, double w, double h) override {
		cairo_save(cr);
		cairo_tag_begin(
			cr, CAIRO_TAG_LINK, ("uri='" + target_uri + "'").c_str());
		child->render(cr, w, h);
		cairo_tag_end(cr, CAIRO_TAG_LINK);
		cairo_restore(cr);
	}
};

// --- Pictures ----------------------------------------------------------------

struct image_info {
	double width = 0., height = 0., dpi_x = 72., dpi_y = 72.;
};

/// http://libpng.org/pub/png/spec/1.2/PNG-Contents.html
static bool read_png_info(image_info &info, const char *data, size_t length) {
	return length >= 24 && !memcmp(data, "\211PNG\r\n\032\n", 8) &&
		!memcmp(data + 12, "IHDR", 4) &&
		(info.width = ntohl(*(uint32_t *) (data + 16))) &&
		(info.height = ntohl(*(uint32_t *) (data + 20)));
}

DefWidget(Picture) {
	double w = 0, h = 0;
	double scale_x = 1., scale_y = 1.;
	cairo_surface_t *surface = nullptr;

	virtual tuple<double, double> prepare(PangoContext *) override {
		return {w * scale_x, h * scale_y};
	}

	virtual void render(cairo_t *cr, double width, double height) override {
		if (!surface || width <= 0 || height <= 0)
			return;

		double ww = this->w * scale_x;
		double hh = this->h * scale_y;
		double postscale = width / ww;
		if (hh * postscale > height)
			postscale = height / hh;

		// For PDF-A, ISO 19005-3:2012 6.2.8: interpolation is not allowed
		// (Cairo sets it on by default).
		bool interpolate = true;

		auto pattern = cairo_pattern_create_for_surface(surface);
		cairo_pattern_set_filter(
			pattern, interpolate ? CAIRO_FILTER_GOOD : CAIRO_FILTER_NEAREST);

		// Maybe we should also center the picture or something...
		cairo_scale(cr, scale_x * postscale, scale_y * postscale);
		cairo_set_source(cr, pattern);
		cairo_paint(cr);

		cairo_pattern_destroy(pattern);
	}

	static cairo_surface_t *make_surface_png(const string &data) {
		using CharRange = pair<const char *, const char *>;
		CharRange iterator{&*data.begin(), &*data.end()};
		return cairo_image_surface_create_from_png_stream(
			[](void *closure, unsigned char *data, uint len) {
				auto i = (CharRange *) closure;
				if (i->second - i->first < len)
					return CAIRO_STATUS_READ_ERROR;

				memcpy(data, i->first, len);
				i->first += len;
				return CAIRO_STATUS_SUCCESS;
			},
			&iterator);
	}

	// Cairo doesn't support PNGs in PDFs by MIME type,
	// until then we'll have to parametrize.
	static function<cairo_surface_t *()> identify(
		const string &picture, image_info &info) {
		if (read_png_info(info, picture.data(), picture.length()))
			return bind(make_surface_png, picture);
		return nullptr;
	}

	Picture(const string &filename) {
		ifstream t{filename};
		stringstream buffer;
		buffer << t.rdbuf();
		string picture = buffer.str();

		image_info info;
		if (auto make_surface = identify(picture, info)) {
			surface = make_surface();
			w = info.width;
			h = info.height;
			scale_x = info.dpi_x / 72.;
			scale_y = info.dpi_y / 72.;
		} else {
			cerr << "warning: unreadable picture: " << filename << endl;
		}
	}
};

// --- QR ----------------------------------------------------------------------

DefWidget(QR) {
	QRcode *code = nullptr;
	double T = 1.;

	QR(string text, double T) : T(T) {
		QRinput *data = QRinput_new2(
			0 /* Version, i.e., size, here autoselect */,
			QR_ECLEVEL_M /* 15% correction */);
		if (!data)
			return;

		auto u8 = reinterpret_cast<const unsigned char *>(text.data());
		(void) QRinput_append(data, !QRinput_check(QR_MODE_AN, text.size(), u8)
			? QR_MODE_AN : QR_MODE_8, text.size(), u8);

		code = QRcode_encodeInput(data);
		QRinput_free(data);
	}

	virtual ~QR() override {
		if (code)
			QRcode_free(code);
	}

	virtual tuple<double, double> prepare([[maybe_unused]] PangoContext *pc)
		override {
		if (!code)
			return {0, 0};

		return {T * code->width, T * code->width};
	}

	virtual void render(cairo_t *cr,
		[[maybe_unused]] double w, [[maybe_unused]] double h) override {
		if (!code)
			return;

		auto line = code->data;
		for (int y = 0; y < code->width; y++) {
			for (int x = 0; x < code->width; x++) {
				if (line[x] & 1)
					cairo_rectangle(cr, T * x, T * y, T, T);
			}
			line += code->width;
		}
		cairo_fill(cr);
	}
};

// --- Lua Widget --------------------------------------------------------------

#define XLUA_WIDGET_METATABLE "widget"

struct LuaWidget {
	// shared_ptr would resolve the reference stealing API design issue.
	unique_ptr<Widget> widget;
};

static void xlua_widget_check(lua_State *L, LuaWidget *self) {
	if (!self->widget)
		luaL_error(L, "trying to use a consumed widget reference");
}

static attribute xlua_widget_tovalue(lua_State *L, LuaWidget *self, int idx) {
	xlua_widget_check(L, self);
	if (lua_isnumber(L, idx))
		return lua_tonumber(L, idx);
	if (lua_isstring(L, idx)) {
		size_t len = 0;
		const char *s = lua_tolstring(L, idx, &len);
		return string(s, len);
	}
	luaL_error(L, "expected string or numeric attributes");
	return {};
}

static void xlua_widget_set(
	lua_State *L, LuaWidget *self, Widget *widget, int idx_attrs) {
	self->widget.reset(widget);
	if (!idx_attrs)
		return;

	lua_pushvalue(L, idx_attrs);
	lua_pushnil(L);
	while (lua_next(L, -2)) {
		if (lua_type(L, -2) == LUA_TSTRING) {
			size_t key_len = 0;
			const char *key = lua_tolstring(L, -2, &key_len);
			widget->setattr(
				string(key, key_len), xlua_widget_tovalue(L, self, -1));
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
}

static int xlua_widget_gc(lua_State *L) {
	auto self = (LuaWidget *) luaL_checkudata(L, 1, XLUA_WIDGET_METATABLE);
	self->widget.reset(nullptr);
	return 0;
}

static int xlua_widget_index(lua_State *L) {
	auto self = (LuaWidget *) luaL_checkudata(L, 1, XLUA_WIDGET_METATABLE);
	// In theory, this could also index container children,
	// but it does not seem practically useful.
	auto key = luaL_checkstring(L, 2);
	xlua_widget_check(L, self);

	if (auto it = self->widget->attributes.find(key);
		it == self->widget->attributes.end())
		lua_pushnil(L);
	else if (auto s = get_if<string>(&it->second))
		lua_pushlstring(L, s->c_str(), s->length());
	else if (auto n = get_if<double>(&it->second))
		lua_pushnumber(L, *n);
	return 1;
}

static int xlua_widget_newindex(lua_State *L) {
	auto self = (LuaWidget *) luaL_checkudata(L, 1, XLUA_WIDGET_METATABLE);
	auto key = luaL_checkstring(L, 2);
	xlua_widget_check(L, self);

	self->widget->attributes[key] = xlua_widget_tovalue(L, self, 3);
	return 0;
}

static luaL_Reg xlua_widget_table[] = {
	{"__gc",       xlua_widget_gc},
	{"__index",    xlua_widget_index},
	{"__newindex", xlua_widget_newindex},
	{}
};

// --- Lua Document ------------------------------------------------------------

#define XLUA_DOCUMENT_METATABLE "document"

struct LuaDocument {
	cairo_t *cr = nullptr;              ///< Cairo
	cairo_surface_t *pdf = nullptr;     ///< PDF surface
	PangoContext *pc = nullptr;         ///< Pango context

	double page_width = 0.;             ///< Page width in 72 DPI points
	double page_height = 0.;            ///< Page height in 72 DPI points
	double page_margin = 0.;            ///< Page margins in 72 DPI points
};

static int xlua_document_gc(lua_State *L) {
	auto self = (LuaDocument *) luaL_checkudata(L, 1, XLUA_DOCUMENT_METATABLE);
	cairo_destroy(self->cr);
	g_object_unref(self->pc);
	return 0;
}

static int xlua_document_index(lua_State *L) {
	if (auto key = luaL_checkstring(L, 2); *key == '_')
		lua_pushnil(L);
	else
		luaL_getmetafield(L, 1, key);
	return 1;
}

// And probably for links as well.
#if CAIRO_VERSION < CAIRO_VERSION_ENCODE(1, 15, 4)
#error "At least Cairo 1.15.4 is required for setting PDF metadata."
#endif

static optional<cairo_pdf_metadata_t> metadata_by_name(const char *name) {
	if (!strcmp(name, "title"))
		return CAIRO_PDF_METADATA_TITLE;
	if (!strcmp(name, "author"))
		return CAIRO_PDF_METADATA_AUTHOR;
	if (!strcmp(name, "subject"))
		return CAIRO_PDF_METADATA_SUBJECT;
	if (!strcmp(name, "keywords"))
		return CAIRO_PDF_METADATA_KEYWORDS;
	if (!strcmp(name, "creator"))
		return CAIRO_PDF_METADATA_CREATOR;
	if (!strcmp(name, "create_date"))
		return CAIRO_PDF_METADATA_CREATE_DATE;
	if (!strcmp(name, "mod_date"))
		return CAIRO_PDF_METADATA_MOD_DATE;
	return {};
}

static int xlua_document_newindex(lua_State *L) {
	auto self = (LuaDocument *) luaL_checkudata(L, 1, XLUA_DOCUMENT_METATABLE);
	auto name = luaL_checkstring(L, 2);
	auto value = luaL_checkstring(L, 3);

	// These are all read-only in Cairo.
	if (auto id = metadata_by_name(name))
		cairo_pdf_surface_set_metadata(self->pdf, id.value(), value);
	else
		return luaL_error(L, "%s: unknown property");
	return 0;
}

static int xlua_document_show(lua_State *L) {
	auto self = (LuaDocument *) luaL_checkudata(L, 1, XLUA_DOCUMENT_METATABLE);
	for (int i = 2; i <= lua_gettop(L); i++) {
		auto w = (LuaWidget *) luaL_checkudata(L, i, XLUA_WIDGET_METATABLE);
		xlua_widget_check(L, w);
		auto widget = w->widget.get();
		widget->apply_attributes();

		auto inner_width = self->page_width - 2 * self->page_margin;
		auto inner_height = self->page_height - 2 * self->page_margin;
		widget->prepare_for_size(self->pc, inner_width, inner_height);

		cairo_save(self->cr);
		cairo_translate(self->cr, self->page_margin, self->page_margin);
		widget->render(self->cr, inner_width, inner_height);
		cairo_restore(self->cr);
	}
	cairo_show_page(self->cr);
	return 0;
}

static luaL_Reg xlua_document_table[] = {
	{"__gc",       xlua_document_gc},
	{"__index",    xlua_document_index},
	{"__newindex", xlua_document_newindex},
	{"show",       xlua_document_show},
	{}
};

// --- Library -----------------------------------------------------------------

// 1 point is 1/72 inch, also applies to PDF surfaces.
static int xlua_cm(lua_State *L) {
	lua_pushnumber(L, luaL_checknumber(L, 1) / 2.54 * 72);
	return 1;
}

struct xlua_numpunct : public numpunct<char> {
	optional<char> thousands_sep_override;
	optional<char> decimal_point_override;
	optional<string_type> grouping_override;

	using super = std::numpunct<char>;

	virtual char do_thousands_sep() const override {
		return thousands_sep_override.value_or(super::do_thousands_sep());
	}

	virtual char do_decimal_point() const override {
		return decimal_point_override.value_or(super::do_decimal_point());
	}

	virtual string_type do_grouping() const override {
		return grouping_override.value_or(super::do_grouping());
	}
};

static int xlua_ntoa(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);

	auto np = new xlua_numpunct();
	const char *field = nullptr;
	if (lua_getfield(L, 1, (field = "thousands_sep")) != LUA_TNIL) {
		size_t len = 0;
		auto str = lua_tolstring(L, -1, &len);
		if (!str || len != 1)
			return luaL_error(L, "invalid %s", field);
		np->thousands_sep_override.emplace(str[0]);
	}
	if (lua_getfield(L, 1, (field = "decimal_point")) != LUA_TNIL) {
		size_t len = 0;
		auto str = lua_tolstring(L, -1, &len);
		if (!str || len != 1)
			return luaL_error(L, "invalid %s", field);
		np->decimal_point_override.emplace(str[0]);
	}
	if (lua_getfield(L, 1, (field = "grouping")) != LUA_TNIL) {
		size_t len = 0;
		auto str = lua_tolstring(L, -1, &len);
		if (!str)
			return luaL_error(L, "invalid %s", field);
		np->grouping_override.emplace(string(str, len));
	}

	ostringstream formatted;
	formatted.imbue(locale(locale(), np));
	if (lua_getfield(L, 1, "precision") != LUA_TNIL) {
		formatted.setf(formatted.fixed, formatted.floatfield);
		formatted.precision(lua_tointeger(L, -1));
	}

	lua_geti(L, 1, 1);
	if (lua_isinteger(L, -1))
		formatted << lua_tointeger(L, -1);
	else if (lua_isnumber(L, -1))
		formatted << lua_tonumber(L, -1);
	else
		return luaL_error(L, "number expected as the first field");

	lua_pushstring(L, formatted.str().c_str());
	return 1;
}

static int xlua_escape(lua_State *L) {
	string escaped;
	for (int i = 1; i <= lua_gettop(L); i++) {
		size_t len = 0;
		const char *s = luaL_checklstring(L, i, &len);
		escaped.append(Text::escape(s, len));
	}
	lua_pushlstring(L, escaped.data(), escaped.length());
	return 1;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static int xlua_document(lua_State *L) {
	const char *filename = luaL_checkstring(L, 1);
	lua_Number width = luaL_checknumber(L, 2);
	lua_Number height = luaL_checknumber(L, 3);

	LuaDocument *self =
		static_cast<LuaDocument *>(lua_newuserdata(L, sizeof *self));
	luaL_setmetatable(L, XLUA_DOCUMENT_METATABLE);
	new(self) LuaDocument;

	self->pdf = cairo_pdf_surface_create(filename,
		(self->page_width = width), (self->page_height = height));
	self->cr = cairo_create(self->pdf);
	cairo_surface_destroy(self->pdf);

	self->page_margin = luaL_optnumber(L, 4, self->page_margin);

	auto pc = self->pc = pango_cairo_create_context(self->cr);
	// By default the resolution is set to 96 DPI but the PDF surface uses 72.
	pango_cairo_context_set_resolution(pc, 72.);

#if PANGO_VERSION_CHECK(1, 44, 0)
	// Otherwise kerning was broken in Pango before 1.48.6.
	// Seems like this issue: https://gitlab.gnome.org/GNOME/pango/-/issues/562
	// and might be related to: https://blogs.gnome.org/mclasen/2019/08/
	pango_context_set_round_glyph_positions(pc, FALSE);
#endif
	return 1;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static LuaWidget *xlua_newwidget(lua_State *L) {
	LuaWidget *self =
		static_cast<LuaWidget *>(lua_newuserdata(L, sizeof *self));
	luaL_setmetatable(L, XLUA_WIDGET_METATABLE);
	new(self) LuaWidget;
	return self;
}

static int xlua_filler(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);

	double width = -1, height = -1;
	if (lua_geti(L, 1, 1); !lua_isnoneornil(L, -1))
		width = lua_tonumber(L, -1);
	if (lua_geti(L, 1, 2); !lua_isnoneornil(L, -1))
		height = lua_tonumber(L, -1);

	auto self = xlua_newwidget(L);
	xlua_widget_set(L, self, new Filler{width, height}, 1);
	return 1;
}

static int xlua_hline(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);

	double thickness = 1;
	if (lua_geti(L, 1, 1); !lua_isnoneornil(L, -1))
		thickness = lua_tonumber(L, -1);

	auto self = xlua_newwidget(L);
	xlua_widget_set(L, self, new HLine{thickness}, 1);
	return 1;
}

static int xlua_vline(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);

	double thickness = 1;
	if (lua_geti(L, 1, 1); !lua_isnoneornil(L, -1))
		thickness = lua_tonumber(L, -1);

	auto self = xlua_newwidget(L);
	xlua_widget_set(L, self, new VLine{thickness}, 1);
	return 1;
}

static string xlua_tostring(lua_State *L, int idx) {
	// Automatic conversions are unlikely to be valid XML.
	bool escape = !lua_isstring(L, idx);

	size_t length = 0;
	const char *s = luaL_tolstring(L, idx, &length);
	string text = escape ? Text::escape(s, length) : string(s, length);
	lua_pop(L, 1);
	return text;
}

static int xlua_text(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);

	string text;
	for (lua_Integer i = 1, len = luaL_len(L, 1); i <= len; i++) {
		lua_geti(L, 1, i);
		text.append(xlua_tostring(L, -1));
		lua_pop(L, 1);
	}

	auto self = xlua_newwidget(L);
	xlua_widget_set(L, self, new Text{text}, 1);
	return 1;
}

static LuaWidget *xlua_towidget(lua_State *L) {
	if (luaL_testudata(L, -1, XLUA_WIDGET_METATABLE))
		return (LuaWidget *) luaL_checkudata(L, -1, XLUA_WIDGET_METATABLE);

	string text = xlua_tostring(L, -1);
	lua_pop(L, 1);

	auto self = xlua_newwidget(L);
	xlua_widget_set(L, self, new Text{text}, 0);
	return self;
}

static int xlua_frame(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	if (luaL_len(L, 1) != 1)
		return luaL_error(L, "expected one child widget");

	lua_geti(L, 1, 1);
	auto child = xlua_towidget(L);

	auto self = xlua_newwidget(L);
	xlua_widget_set(L, self, new Frame{child->widget.release()}, 1);
	return 1;
}

static int xlua_link(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	if (luaL_len(L, 1) != 2)
		return luaL_error(L, "expected link target and one child widget");

	lua_geti(L, 1, 1);
	size_t length = 0;
	const char *s = luaL_tolstring(L, -1, &length);
	string target(s, length);
	lua_pop(L, 1);

	lua_geti(L, 1, 2);
	auto child = xlua_towidget(L);

	auto self = xlua_newwidget(L);
	xlua_widget_set(L, self, new Link{target, child->widget.release()}, 1);
	return 1;
}

static int xlua_hbox(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);

	vector<unique_ptr<Widget>> children;
	for (lua_Integer i = 1, len = luaL_len(L, 1); i <= len; i++) {
		lua_geti(L, 1, i);
		children.emplace_back(xlua_towidget(L)->widget.release());
		lua_pop(L, 1);
	}

	auto self = xlua_newwidget(L);
	xlua_widget_set(L, self, new HBox{std::move(children)}, 1);
	return 1;
}

static int xlua_vbox(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);

	vector<unique_ptr<Widget>> children;
	for (lua_Integer i = 1, len = luaL_len(L, 1); i <= len; i++) {
		lua_geti(L, 1, i);
		children.emplace_back(xlua_towidget(L)->widget.release());
		lua_pop(L, 1);
	}

	auto self = xlua_newwidget(L);
	xlua_widget_set(L, self, new VBox{std::move(children)}, 1);
	return 1;
}

static int xlua_picture(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	if (luaL_len(L, 1) != 1)
		return luaL_error(L, "expected picture path");

	lua_geti(L, 1, 1);
	size_t length = 0;
	const char *s = luaL_tolstring(L, -1, &length);
	string filename(s, length);
	lua_pop(L, 1);

	auto self = xlua_newwidget(L);
	xlua_widget_set(L, self, new Picture{filename}, 1);
	return 1;
}

static int xlua_qr(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	if (luaL_len(L, 1) != 2)
		return luaL_error(L, "expected contents and module size");

	lua_geti(L, 1, 1);
	size_t length = 0;
	const char *s = luaL_tolstring(L, -1, &length);
	string target(s, length);
	lua_pop(L, 1);

	lua_geti(L, 1, 2);
	auto T = lua_tonumber(L, -1);
	lua_pop(L, 1);

	auto self = xlua_newwidget(L);
	xlua_widget_set(L, self, new QR{target, T}, 1);
	return 1;
}

static luaL_Reg xlua_library[] = {
	{"cm",       xlua_cm},
	{"ntoa",     xlua_ntoa},
	{"escape",   xlua_escape},

	{"Document", xlua_document},

	{"Filler",   xlua_filler},
	{"HLine",    xlua_hline},
	{"VLine",    xlua_vline},
	{"Text",     xlua_text},
	{"Frame",    xlua_frame},
	{"Link",     xlua_link},
	{"HBox",     xlua_hbox},
	{"VBox",     xlua_vbox},
	{"Picture",  xlua_picture},
	{"QR",       xlua_qr},
	{}
};

// --- Initialisation, event handling ------------------------------------------

static int xlua_error_handler(lua_State *L) {
	// Don't add tracebacks when there's already one, and pass nil through.
	const char *string = luaL_optstring(L, 1, NULL);
	if (string && !strchr(string, '\n')) {
		luaL_traceback(L, L, string, 1);
		lua_remove(L, 1);
	}
	return 1;
}

static void *xlua_alloc([[maybe_unused]] void *ud, void *ptr,
	[[maybe_unused]] size_t o_size, size_t n_size) {
	if (n_size)
		return realloc(ptr, n_size);

	free(ptr);
	return NULL;
}

static int xlua_panic(lua_State *L) {
	cerr << "fatal: Lua panicked: " << lua_tostring(L, -1) << endl;
	lua_close(L);
	exit(EXIT_FAILURE);
	return 0;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		cerr << "Usage: " << argv[0] << " program.lua [args...]" << endl;
		return 1;
	}

	lua_State *L = lua_newstate(xlua_alloc, NULL);
	if (!L) {
		cerr << "fatal: Lua initialization failed" << endl;
		return 1;
	}
	lua_atpanic(L, xlua_panic);
	luaL_openlibs(L);
	luaL_checkversion(L);

	luaL_newlib(L, xlua_library);
	lua_setglobal(L, "lpg");

	luaL_newmetatable(L, XLUA_DOCUMENT_METATABLE);
	luaL_setfuncs(L, xlua_document_table, 0);
	lua_pop(L, 1);

	luaL_newmetatable(L, XLUA_WIDGET_METATABLE);
	luaL_setfuncs(L, xlua_widget_table, 0);
	lua_pop(L, 1);

	luaL_checkstack(L, argc, NULL);

	// Joining the first two might make a tiny bit more sense.
	lua_createtable(L, argc - 1, 0);
	lua_pushstring(L, (string(argv[0]) + " " + argv[1]).c_str());
	lua_rawseti(L, 1, 1);
	for (int i = 2; i < argc; i++) {
		lua_pushstring(L, argv[i]);
		lua_rawseti(L, 1, i - 1);
	}
	lua_setglobal(L, "arg");

	int status = 0;
	lua_pushcfunction(L, xlua_error_handler);
	if ((status = luaL_loadfile(L, strcmp(argv[1], "-") ? argv[1] : NULL)))
		goto error;
	for (int i = 2; i < argc; i++)
		lua_pushstring(L, argv[i]);
	if ((status = lua_pcall(L, argc - 2, 0, 1)))
		goto error;
	lua_close(L);
	return 0;

error:
	// Lua will unfortunately discard exceptions that it hasn't thrown itself.
	if (const char *err = lua_tostring(L, -1))
		cerr << "error: " << err << endl;
	else
		cerr << "error: " << status << endl;
	lua_close(L);
	return 1;
}
