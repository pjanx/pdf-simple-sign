#!/usr/bin/env lpg
local project_url = "https://git.janouch.name/p/pdf-simple-sign"

function h1 (title)
	return lpg.VBox {fontsize=18., fontweight=600,
		title, lpg.HLine {2}, lpg.Filler {-1, 6}}
end
function h2 (title)
	return lpg.VBox {fontsize=16., fontweight=600,
		lpg.Filler {-1, 8}, title, lpg.HLine {1}, lpg.Filler {-1, 6}}
end
function h3 (title)
	return lpg.VBox {fontsize=14., fontweight=600,
		lpg.Filler {-1, 8}, title, lpg.HLine {.25}, lpg.Filler {-1, 6}}
end
function p (...)
	return lpg.VBox {..., lpg.Filler {-1, 6}}
end
function code (...)
	return lpg.VBox {
		lpg.Filler {-1, 4},
		lpg.HBox {
			lpg.Filler {12},
			lpg.VBox {"<tt>" .. table.concat {...} .. "</tt>"},
			lpg.Filler {},
		},
		lpg.Filler {-1, 6},
	}
end
function define (name, ...)
	return lpg.VBox {
		lpg.Filler {-1, 2},
		lpg.Text {fontweight=600, name}, lpg.Filler {-1, 2},
		lpg.HBox {lpg.Filler {12}, lpg.VBox {...}, lpg.Filler {}},
		lpg.Filler {-1, 2},
	}
end
function pad (widget)
	return lpg.VBox {
		lpg.Filler {-1, 2},
		lpg.HBox {lpg.Filler {4}, widget, lpg.Filler {}, lpg.Filler {4}},
		lpg.Filler {-1, 2},
	}
end

local page1 = lpg.VBox {fontfamily="sans serif", fontsize=12.,
	h1("lpg User Manual"),
	p("<b>lpg</b> is a Lua-based PDF document generator, exposing a trivial " ..
		"layouting engine on top of the Cairo graphics library, " ..
		"with manual paging."),
	p("The author has primarily been using this system to typeset invoices."),

	h2("Synopsis"),
	p("<b>lpg</b> <i>program.lua</i> [<i>args...</i>]"),

	h2("API"),
	p("The Lua program receives <b>lpg</b>'s and its own path joined " ..
		"as <tt>arg[0]</tt>.  Any remaining sequential elements " ..
		"of this table represent the passed <i>args</i>."),

	h3("Utilities"),

	define("lpg.cm (centimeters)",
		p("Returns how many document points are needed " ..
			"for the given physical length.")),

	define("lpg.ntoa {number [, precision=…]\n" ..
		"\t[, thousands_sep=…] [, decimal_point=…] [, grouping=…]}",
		p("Formats a number using the C++ localization " ..
			"and I/O libraries.  " ..
			"For example, the following call results in “3 141,59”:"),
		code("ntoa {3141.592, precision=2,\n" ..
			"  thousands_sep=\" \", decimal_point=\",\", " ..
			"grouping=\"\\003\"}")),

	define("lpg.escape (values...)",
		p("Interprets all values as strings, " ..
			"and escapes them to be used as literal text—" ..
			"all text within <b>lpg</b> is parsed as Pango markup, " ..
			"which is a subset of XML.")),

	h3("PDF documents"),

	define("lpg.Document (filename, width, height [, margin])",
		p("Returns a new <i>Document</i> object, whose pages are all " ..
			"the same size in 72 DPI points, as specified by <b>width</b> " ..
			"and <b>height</b>.  The <b>margin</b> is used by <b>show</b> " ..
			"on all sides of pages."),
		p("The file is finalized when the object is garbage collected.")),

	define("<i>Document</i>.title, author, subject, keywords, " ..
		"creator, create_date, mod_date",
		p("Write-only PDF <i>Info</i> dictionary metadata strings.")),

	define("<i>Document</i>:show ([widget...])",
		p("Starts a new document page, and renders <i>Widget</i> trees over " ..
		"the whole print area.")),

	lpg.Filler {},
}

local page2 = lpg.VBox {fontfamily="sans serif", fontsize=12.,
	h3("Widgets"),
	p("The layouting system makes heavy use of composition, " ..
		"and thus stays simple."),
	p("For convenience, anywhere a <i>Widget</i> is expected but another " ..
		"kind of value is received, <b>lpg.Text</b> widget will be invoked " ..
		"on that value."),
	p("Once a <i>Widget</i> is included in another <i>Widget</i>, " ..
		"the original Lua object can no longer be used, " ..
		"as its reference has been consumed."),
	p("<i>Widgets</i> can be indexed by strings to get or set " ..
		"their <i>attributes</i>.  All <i>Widget</i> constructor tables " ..
		"also accept attributes, for convenience.  Attributes can be " ..
		"either strings or numbers, mostly only act " ..
		"on specific <i>Widget</i> kinds, and are hereditary.  " ..
		"Prefix their names with an underscore to set them ‘privately’."),
	p("<i>Widget</i> sizes can be set negative, which signals to their " ..
		"container that they should take any remaining space, " ..
		"after all their siblings’ requests have been satisfied.  " ..
		"When multiple widgets make this request, that space is distributed " ..
		"in proportion to these negative values."),

	define("lpg.Filler {[width] [, height]}",
		p("Returns a new blank widget with the given dimensions, " ..
			"which default to -1, -1.")),
	define("lpg.HLine {[thickness]}",
		p("Returns a new widget that draws a simple horizontal line " ..
			"of the given <b>thickness</b>.")),
	define("lpg.VLine {[thickness]}",
		p("Returns a new widget that draws a simple vertical line " ..
			"of the given <b>thickness</b>.")),
	define("lpg.Text {[value...]}",
		p("Returns a new text widget that renders the concatenation of all " ..
			"passed values filtered through Lua’s <b>tostring</b> " ..
			"function.  Non-strings will additionally be escaped."),
		define("<i>Text</i>.fontfamily, fontsize, fontweight, lineheight",
			p("Various font properties, similar to their CSS counterparts."))),
	define("lpg.Frame {widget}",
		p("Returns a special container widget that can override " ..
			"a few interesting properties."),
		define("<i>Frame</i>.color",
			p("Text and line colour, for example <tt>0xff0000</tt> for red.")),
		define("<i>Frame</i>.w_override",
			p("Forcefully changes the child <i>Widget</i>’s " ..
				"requested width, such as to negative values.")),
		define("<i>Frame</i>.h_override",
			p("Forcefully changes the child <i>Widget</i>’s " ..
				"requested height, such as to negative values."))),

	lpg.Filler {},
}

local page3 = lpg.VBox {fontfamily="sans serif", fontsize=12.,
	define("lpg.Link {target, widget}",
		p("Returns a new hyperlink widget pointing to the <b>target</b>, " ..
			"which is a URL.  The hyperlink applies " ..
			"to the entire area of the child widget.  " ..
			"It has no special appearance.")),
	define("lpg.HBox {[widget...]}",
		p("Returns a new container widget that places children " ..
			"horizontally, from left to right."),
		p("If any space remains after satisfying the children widgets’ " ..
			"requisitions, it is distributed equally amongst all of them.  " ..
			"Also see the note about negative sizes.")),
	define("lpg.VBox {[widget...]}",
		p("Returns a new container widget that places children " ..
			"vertically, from top to bottom.")),
	define("lpg.Picture {filename}",
		p("Returns a new picture widget, showing the given <b>filename</b>, " ..
			"which currently must be in the PNG format.  " ..
			"Pictures are rescaled to fit, but keep their aspect ratio.")),
	define("lpg.QR {contents, module}",
		p("Returns a new QR code widget, encoding the <b>contents</b> " ..
			"string using the given <b>module</b> size.  " ..
			"The QR code version is chosen automatically.")),
	
	h2("Examples"),
	p("See the source code of this user manual " ..
		"for the general structure of scripts."),

	h3("Size distribution and composition"),
	lpg.VBox {
		lpg.HLine {},
		lpg.HBox {
			lpg.VLine {}, lpg.Frame {_w_override=lpg.cm(3), pad "3cm"},
			lpg.VLine {}, lpg.Frame {pad "Measured"},
			lpg.VLine {}, lpg.Frame {_w_override=-1, pad "-1"},
			lpg.VLine {}, lpg.Frame {_w_override=-2, pad "-2"},
			lpg.VLine {},
		},
		lpg.HLine {},
	},
	lpg.Filler {-1, 6},
	code([[
<small><b>function</b> pad (widget)
  <b>local function</b> f (...) <b>return</b> lpg.Filler {...} <b>end</b>
  <b>return</b> lpg.VBox {f(-1, 2), lpg.HBox {f(4), w, f(), f(4)}, f(-1, 2)}
<b>end</b>

lpg.VBox {lpg.HLine {}, lpg.HBox {
  lpg.VLine {}, lpg.Frame {_w_override=lpg.cm(3), pad "3cm"},
  lpg.VLine {}, lpg.Frame {pad "Measured"},
  lpg.VLine {}, lpg.Frame {_w_override=-1, pad "-1"},
  lpg.VLine {}, lpg.Frame {_w_override=-2, pad "-2"},
  lpg.VLine {},
}, lpg.HLine {}}</small>]]),

	h3("Clickable QR code link"),
	lpg.HBox {
		lpg.VBox {
			p("Go here to report bugs, request features, " ..
				"or submit pull requests:"),
			code(([[
url = "%s"
lpg.Link {url, lpg.QR {url, 2.5}}]]):format(project_url)),
		},
		lpg.Filler {},
		lpg.Link {project_url, lpg.QR {project_url, 2.5}},
	},

	lpg.Filler {},
}

if #arg < 1 then
	io.stderr:write("Usage: " .. arg[0] .. " OUTPUT-PDF..." .. "\n")
	os.exit(false)
end
local width, height, margin = lpg.cm(21), lpg.cm(29.7), lpg.cm(2.0)
for i = 1, #arg do
	local pdf = lpg.Document(arg[i], width, height, margin)
	pdf.title = "lpg User Manual"
	pdf.subject = "lpg User Manual"
	pdf.author = "Přemysl Eric Janouch"
	pdf.creator = ("lpg (%s)"):format(project_url)

	pdf:show(page1)
	pdf:show(page2)
	pdf:show(page3)
end
