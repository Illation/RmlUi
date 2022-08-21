/*
 * This source file is part of RmlUi, the HTML/CSS Interface Middleware
 *
 * For the latest information, see http://github.com/mikke89/RmlUi
 *
 * Copyright (c) 2008-2010 CodePoint Ltd, Shift Technology Ltd
 * Copyright (c) 2019 The RmlUi Team, and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "DecoratorGradient.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/GeometryUtilities.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/RenderInterface.h"
#include "ComputeProperty.h"
#include "DecoratorElementData.h"
#include <algorithm>

namespace Rml {

/// Convert all color stop positions to normalized numbers.
/// @param[in] element The element to resolve lengths against.
/// @param[in] gradient_line_length The length of the gradient line, along which color stops are placed.
/// @param[in] soft_spacing The desired minimum distance between stops to avoid aliasing, in normalized number units.
/// @param[in] unresolved_stops
/// @return A list of resolved color stops, all in number units.
static ColorStopList ResolveColorStops(Element* element, const float gradient_line_length, const float soft_spacing,
	const ColorStopList& unresolved_stops)
{
	ColorStopList stops = unresolved_stops;
	const int num_stops = (int)stops.size();

	// Resolve all lengths and percentages to numbers. After this step all stops with a unit other than Number are considered as Auto.
	for (ColorStop& stop : stops)
	{
		if (Any(stop.position.unit & Unit::LENGTH))
		{
			const float resolved_position = element->ResolveLength(stop.position);
			stop.position = NumericValue(resolved_position / gradient_line_length, Unit::NUMBER);
		}
		else if (stop.position.unit == Unit::PERCENT)
		{
			stop.position = NumericValue(stop.position.number * 0.01f, Unit::NUMBER);
		}
	}

	// Resolve auto positions of the first and last color stops.
	auto resolve_edge_stop = [](ColorStop& stop, float auto_to_number) {
		if (stop.position.unit != Unit::NUMBER)
			stop.position = NumericValue(auto_to_number, Unit::NUMBER);
	};
	resolve_edge_stop(stops[0], 0.f);
	resolve_edge_stop(stops[num_stops - 1], 1.f);

	// Ensures that color stop positions are strictly increasing, and have at least 1px spacing to avoid aliasing.
	auto nudge_stop = [prev_position = stops[0].position.number](ColorStop& stop, bool update_prev = true) mutable {
		stop.position.number = Math::Max(stop.position.number, prev_position);
		if (update_prev)
			prev_position = stop.position.number;
	};
	int auto_begin_i = -1;

	// Evenly space stops with sequential auto indices, and nudge stop positions to ensure strictly increasing positions.
	for (int i = 1; i < num_stops; i++)
	{
		ColorStop& stop = stops[i];
		if (stop.position.unit != Unit::NUMBER)
		{
			// Mark the first of any consecutive auto stops.
			if (auto_begin_i < 0)
				auto_begin_i = i;
		}
		else if (auto_begin_i < 0)
		{
			// The stop has a definite position and there are no previous autos to handle, just ensure it is properly spaced.
			nudge_stop(stop);
		}
		else
		{
			// Space out all the previous auto stops, indices [auto_begin_i, i).
			nudge_stop(stop, false);
			const int num_auto_stops = i - auto_begin_i;
			const float t0 = stops[auto_begin_i - 1].position.number;
			const float t1 = stop.position.number;

			for (int j = 0; j < num_auto_stops; j++)
			{
				const float fraction_along_t0_t1 = float(j + 1) / float(num_auto_stops + 1);
				stops[j + auto_begin_i].position = NumericValue(t0 + (t1 - t0) * fraction_along_t0_t1, Unit::NUMBER);
				nudge_stop(stops[j + auto_begin_i]);
			}

			nudge_stop(stop);
			auto_begin_i = -1;
		}
	}

	// Ensures that stops are placed some minimum distance from each other to avoid aliasing, if possible.
	for (int i = 1; i < num_stops - 1; i++)
	{
		const float p0 = stops[i - 1].position.number;
		const float p1 = stops[i].position.number;
		const float p2 = stops[i + 1].position.number;
		float& new_position = stops[i].position.number;

		if (p1 - p0 < soft_spacing)
		{
			if (p2 - p0 < 2.f * soft_spacing)
				new_position = 0.5f * (p2 + p0);
			else
				new_position = p0 + soft_spacing;
		}
	}

	RMLUI_ASSERT(std::all_of(stops.begin(), stops.end(), [](auto&& stop) { return stop.position.unit == Unit::NUMBER; }));

	return stops;
}

/**
    Straight gradient.

    Usage in CSS:
    decorator: gradient( direction start-color stop-color );

    direction: horizontal|vertical;
    start-color: #ff00ff;
    stop-color: #00ff00;
 */
DecoratorGradient::DecoratorGradient() {}

DecoratorGradient::~DecoratorGradient() {}

bool DecoratorGradient::Initialise(const Direction dir_, const Colourb start_, const Colourb stop_)
{
	dir = dir_;
	start = start_;
	stop = stop_;
	return true;
}

DecoratorDataHandle DecoratorGradient::GenerateElementData(Element* element, BoxArea box_area) const
{
	Geometry* geometry = new Geometry(element);
	const Box& box = element->GetBox();

	const ComputedValues& computed = element->GetComputedValues();
	const float opacity = computed.opacity();

	GeometryUtilities::GenerateBackground(geometry, element->GetBox(), Vector2f(0), computed.border_radius(), Colourb(255), box_area);

	// Apply opacity
	Colourb colour_start = start;
	colour_start.alpha = (byte)(opacity * (float)colour_start.alpha);
	Colourb colour_stop = stop;
	colour_stop.alpha = (byte)(opacity * (float)colour_stop.alpha);

	const Vector2f render_offset = box.GetPosition(box_area);
	const Vector2f render_size = box.GetSize(box_area);

	Vector<Vertex>& vertices = geometry->GetVertices();

	if (dir == Direction::Horizontal)
	{
		for (int i = 0; i < (int)vertices.size(); i++)
		{
			const float t = Math::Clamp((vertices[i].position.x - render_offset.x) / render_size.x, 0.0f, 1.0f);
			vertices[i].colour = Math::RoundedLerp(t, colour_start, colour_stop);
		}
	}
	else if (dir == Direction::Vertical)
	{
		for (int i = 0; i < (int)vertices.size(); i++)
		{
			const float t = Math::Clamp((vertices[i].position.y - render_offset.y) / render_size.y, 0.0f, 1.0f);
			vertices[i].colour = Math::RoundedLerp(t, colour_start, colour_stop);
		}
	}

	return reinterpret_cast<DecoratorDataHandle>(geometry);
}

void DecoratorGradient::ReleaseElementData(DecoratorDataHandle element_data) const
{
	delete reinterpret_cast<Geometry*>(element_data);
}

void DecoratorGradient::RenderElement(Element* element, DecoratorDataHandle element_data) const
{
	auto* data = reinterpret_cast<Geometry*>(element_data);
	data->Render(element->GetAbsoluteOffset(BoxArea::Border));
}

/**
    Straight gradient instancer.
 */
DecoratorGradientInstancer::DecoratorGradientInstancer() : DecoratorInstancer(DecoratorClass::Image)
{
	ids.direction = RegisterProperty("direction", "horizontal").AddParser("keyword", "horizontal, vertical").GetId();
	ids.start = RegisterProperty("start-color", "#ffffff").AddParser("color").GetId();
	ids.stop = RegisterProperty("stop-color", "#ffffff").AddParser("color").GetId();
	RegisterShorthand("decorator", "direction, start-color, stop-color", ShorthandType::FallThrough);
}

DecoratorGradientInstancer::~DecoratorGradientInstancer() {}

SharedPtr<Decorator> DecoratorGradientInstancer::InstanceDecorator(const String& /*name*/, const PropertyDictionary& properties_,
	const DecoratorInstancerInterface& /*interface*/)
{
	DecoratorGradient::Direction dir = (DecoratorGradient::Direction)properties_.GetProperty(ids.direction)->Get<int>();
	Colourb start = properties_.GetProperty(ids.start)->Get<Colourb>();
	Colourb stop = properties_.GetProperty(ids.stop)->Get<Colourb>();

	auto decorator = MakeShared<DecoratorGradient>();
	if (decorator->Initialise(dir, start, stop))
	{
		return decorator;
	}

	return nullptr;
}

/**
    Linear gradient.
 */
DecoratorLinearGradient::DecoratorLinearGradient() {}

DecoratorLinearGradient::~DecoratorLinearGradient() {}

bool DecoratorLinearGradient::Initialise(bool in_repeating, float in_angle, const ColorStopList& in_color_stops)
{
	repeating = in_repeating;
	angle = in_angle;
	color_stops = in_color_stops;
	return !color_stops.empty();
}

// Returns the point along the input line ('line_point', 'line_vector') closest to the input 'point'.
static Vector2f IntersectionPointToLineNormal(const Vector2f point, const Vector2f line_point, const Vector2f line_vector)
{
	const Vector2f delta = line_point - point;
	return line_point - delta.DotProduct(line_vector) * line_vector;
}

struct LinearGradientShape {
	Vector2f p0, p1;
	float length;
};

// Find the starting and ending points for the gradient line with the given angle and dimensions.
static LinearGradientShape CalculateLinearGradientShape(float angle, Vector2f dim)
{
	enum { TOP_RIGHT, BOTTOM_RIGHT, BOTTOM_LEFT, TOP_LEFT, COUNT };
	const Vector2f corners[COUNT] = {Vector2f(dim.x, 0), dim, Vector2f(0, dim.y), Vector2f(0, 0)};
	const Vector2f center = 0.5f * dim;

	using uint = unsigned int;
	const uint quadrant = uint(Math::NormaliseAnglePositive(angle) * (4.f / (2.f * Math::RMLUI_PI))) % 4u;
	const uint quadrant_opposite = (quadrant + 2u) % 4u;

	const Vector2f line_vector = Vector2f(Math::Sin(angle), -Math::Cos(angle));
	const Vector2f starting_point = IntersectionPointToLineNormal(corners[quadrant_opposite], center, line_vector);
	const Vector2f ending_point = IntersectionPointToLineNormal(corners[quadrant], center, line_vector);

	const float distance = Math::AbsoluteValue(dim.x * line_vector.x) + Math::AbsoluteValue(-dim.y * line_vector.y);

	return {starting_point, ending_point, distance};
}

DecoratorDataHandle DecoratorLinearGradient::GenerateElementData(Element* element, BoxArea box_area) const
{
	RenderInterface* render_interface = element->GetRenderInterface();
	if (!render_interface)
		return INVALID_DECORATORDATAHANDLE;

	RMLUI_ASSERT(!color_stops.empty());

	const Box& box = element->GetBox();
	const Vector2f dimensions = box.GetSize(box_area);
	LinearGradientShape gradient_shape = CalculateLinearGradientShape(angle, dimensions);

	// One-pixel minimum color stop spacing to avoid aliasing.
	const float soft_spacing = 1.f / gradient_shape.length;

	ColorStopList resolved_stops = ResolveColorStops(element, gradient_shape.length, soft_spacing, color_stops);

	CompiledShaderHandle effect_handle = render_interface->CompileShader("linear-gradient",
		Dictionary{
			{"angle", Variant(angle)},
			{"p0", Variant(gradient_shape.p0)},
			{"p1", Variant(gradient_shape.p1)},
			{"length", Variant(gradient_shape.length)},
			{"repeating", Variant(repeating)},
			{"color_stop_list", Variant(std::move(resolved_stops))},
		});

	Geometry geometry(render_interface);

	const ComputedValues& computed = element->GetComputedValues();
	const byte alpha = byte(computed.opacity() * 255.f);
	GeometryUtilities::GenerateBackground(&geometry, box, Vector2f(), computed.border_radius(), Colourb(255, alpha), box_area);

	const Vector2f render_offset = box.GetPosition(box_area);
	for (Vertex& vertex : geometry.GetVertices())
		vertex.tex_coord = vertex.position - render_offset;

	BasicEffectElementData* element_data = GetBasicEffectElementDataPool().AllocateAndConstruct(std::move(geometry), effect_handle);
	return reinterpret_cast<DecoratorDataHandle>(element_data);
}

void DecoratorLinearGradient::ReleaseElementData(DecoratorDataHandle handle) const
{
	BasicEffectElementData* element_data = reinterpret_cast<BasicEffectElementData*>(handle);
	RenderInterface* render_interface = element_data->geometry.GetRenderInterface();
	render_interface->ReleaseCompiledShader(element_data->effect);

	GetBasicEffectElementDataPool().DestroyAndDeallocate(element_data);
}

void DecoratorLinearGradient::RenderElement(Element* element, DecoratorDataHandle handle) const
{
	BasicEffectElementData* element_data = reinterpret_cast<BasicEffectElementData*>(handle);
	element_data->geometry.Render(element_data->effect, element->GetAbsoluteOffset(BoxArea::Border));
}

/**
    Linear gradient instancer.
 */
DecoratorLinearGradientInstancer::DecoratorLinearGradientInstancer() : DecoratorInstancer(DecoratorClass::Image)
{
	ids.angle = RegisterProperty("angle", "180deg").AddParser("angle").GetId();
	ids.color_stop_list = RegisterProperty("color-stops", "").AddParser("color_stop_list").GetId();

	RegisterShorthand("decorator", "angle?, color-stops#", ShorthandType::RecursiveCommaSeparated);
}

DecoratorLinearGradientInstancer::~DecoratorLinearGradientInstancer() {}

SharedPtr<Decorator> DecoratorLinearGradientInstancer::InstanceDecorator(const String& name, const PropertyDictionary& properties_,
	const DecoratorInstancerInterface& /*interface_*/)
{
	const Property* p_angle = properties_.GetProperty(ids.angle);
	if (!p_angle || !Any(p_angle->unit & Unit::ANGLE))
		return nullptr;
	const Property* p_color_stop_list = properties_.GetProperty(ids.color_stop_list);
	if (!p_color_stop_list || p_color_stop_list->unit != Unit::COLORSTOPLIST)
		return nullptr;

	const float angle = ComputeAngle(p_angle->GetNumericValue());
	const bool repeating = (name == "repeating-linear-gradient");

	const ColorStopList& color_stop_list = p_color_stop_list->value.GetReference<ColorStopList>();

	auto decorator = MakeShared<DecoratorLinearGradient>();
	if (decorator->Initialise(repeating, angle, color_stop_list))
		return decorator;

	return nullptr;
}

/**
    Radial gradient.
 */
DecoratorRadialGradient::DecoratorRadialGradient() {}

DecoratorRadialGradient::~DecoratorRadialGradient() {}

bool DecoratorRadialGradient::Initialise(bool in_repeating, RadialGradient::Shape in_shape, RadialGradient::Size in_size,
	RadialGradient::Position in_position, const ColorStopList& in_color_stops)
{
	repeating = in_repeating;
	shape = in_shape;
	size = in_size;
	position = in_position;
	color_stops = in_color_stops;
	return !color_stops.empty();
}

struct RadialGradientShape {
	Vector2f center, radius;
};
// Find the starting and ending points for the gradient line with the given angle and dimensions.
static RadialGradientShape CalculateRadialGradientShape(Element* element, RadialGradient::Shape shape, RadialGradient::Size size,
	RadialGradient::Position position, Vector2f dimensions)
{
	RadialGradientShape result;
	result.center.x = element->ResolveNumericValue(position.x, dimensions.x);
	result.center.y = element->ResolveNumericValue(position.y, dimensions.y);
	const bool is_circle = (shape == RadialGradient::Shape::Circle);

	auto Abs = [](Vector2f v) { return Vector2f{Math::AbsoluteValue(v.x), Math::AbsoluteValue(v.y)}; };
	auto d = dimensions;
	auto c = result.center;
	Vector2f r;

	switch (size.type)
	{
	case RadialGradient::SizeType::ClosestSide:
	{
		r = Abs(Math::Min(c, d - c));
		result.radius = (is_circle ? Vector2f(Math::Min(r.x, r.y)) : r);
	}
	break;
	case RadialGradient::SizeType::FarthestSide:
	{
		r = Abs(Math::Max(c, d - c));
		result.radius = (is_circle ? Vector2f(Math::Max(r.x, r.y)) : r);
	}
	break;
	case RadialGradient::SizeType::ClosestCorner:
	case RadialGradient::SizeType::FarthestCorner:
	{
		if (size.type == RadialGradient::SizeType::ClosestCorner)
			r = Abs(Math::Min(c, d - c)); // Same as closest-side.
		else
			r = Abs(Math::Max(c, d - c)); // Same as farthest-side.

		if (is_circle)
		{
			result.radius = Vector2f(r.Magnitude());
		}
		else
		{
			r = Math::Max(r, Vector2f(1)); // In case r.x ~= 0
			result.radius.x = Math::SquareRoot(2.f * r.x * r.x);
			result.radius.y = result.radius.x * (r.y / r.x);
		}
	}
	break;
	case RadialGradient::SizeType::LengthPercentage:
	{
		result.radius.x = element->ResolveNumericValue(size.x, d.x);
		result.radius.y = (is_circle ? result.radius.x : element->ResolveNumericValue(size.y, d.y));
		result.radius = Abs(result.radius);
	}
	break;
	}

	result.radius = Math::Max(result.radius, Vector2f(1.f));
	return result;
}

DecoratorDataHandle DecoratorRadialGradient::GenerateElementData(Element* element, BoxArea box_area) const
{
	RenderInterface* render_interface = element->GetRenderInterface();
	if (!render_interface)
		return INVALID_DECORATORDATAHANDLE;

	RMLUI_ASSERT(!color_stops.empty());

	const Box& box = element->GetBox();
	const Vector2f dimensions = box.GetSize(box_area);

	RadialGradientShape gradient_shape = CalculateRadialGradientShape(element, shape, size, position, dimensions);

	// One-pixel minimum color stop spacing to avoid aliasing.
	const float soft_spacing = 1.f / Math::Min(gradient_shape.radius.x, gradient_shape.radius.y);

	ColorStopList resolved_stops = ResolveColorStops(element, gradient_shape.radius.x, soft_spacing, color_stops);

	CompiledShaderHandle effect_handle = render_interface->CompileShader("radial-gradient",
		Dictionary{
			{"center", Variant(gradient_shape.center)},
			{"radius", Variant(gradient_shape.radius)},
			{"repeating", Variant(repeating)},
			{"color_stop_list", Variant(std::move(resolved_stops))},
		});

	Geometry geometry(render_interface);

	const ComputedValues& computed = element->GetComputedValues();
	const byte alpha = byte(computed.opacity() * 255.f);
	GeometryUtilities::GenerateBackground(&geometry, box, Vector2f(), computed.border_radius(), Colourb(255, alpha), box_area);

	const Vector2f render_offset = box.GetPosition(box_area);
	for (Vertex& vertex : geometry.GetVertices())
		vertex.tex_coord = vertex.position - render_offset;

	BasicEffectElementData* element_data = GetBasicEffectElementDataPool().AllocateAndConstruct(std::move(geometry), effect_handle);
	return reinterpret_cast<DecoratorDataHandle>(element_data);
}

void DecoratorRadialGradient::ReleaseElementData(DecoratorDataHandle handle) const
{
	BasicEffectElementData* element_data = reinterpret_cast<BasicEffectElementData*>(handle);
	RenderInterface* render_interface = element_data->geometry.GetRenderInterface();
	render_interface->ReleaseCompiledShader(element_data->effect);

	GetBasicEffectElementDataPool().DestroyAndDeallocate(element_data);
}

void DecoratorRadialGradient::RenderElement(Element* element, DecoratorDataHandle handle) const
{
	BasicEffectElementData* element_data = reinterpret_cast<BasicEffectElementData*>(handle);
	element_data->geometry.Render(element_data->effect, element->GetAbsoluteOffset(BoxArea::Border));
}

/**
    Radial gradient instancer.
 */
DecoratorRadialGradientInstancer::DecoratorRadialGradientInstancer() : DecoratorInstancer(DecoratorClass::Image)
{
	ids.ending_shape = RegisterProperty("ending-shape", "unspecified").AddParser("keyword", "circle, ellipse, unspecified").GetId();

	ids.size_x = RegisterProperty("size-x", "farthest-corner")
					 .AddParser("keyword", "closest-side, farthest-side, closest-corner, farthest-corner")
					 .AddParser("length_percent")
					 .GetId();
	ids.size_y = RegisterProperty("size-y", "unspecified").AddParser("keyword", "unspecified").AddParser("length_percent").GetId();

	RegisterProperty("at", "unspecified").AddParser("keyword", "at, unspecified");
	ids.position_x = RegisterProperty("position-x", "center").AddParser("keyword", "left, center, right").AddParser("length_percent").GetId();
	ids.position_y = RegisterProperty("position-y", "center").AddParser("keyword", "top, center, bottom").AddParser("length_percent").GetId();

	ids.color_stop_list = RegisterProperty("color-stops", "").AddParser("color_stop_list").GetId();

	RegisterShorthand("shape", "ending-shape, size-x, size-y, at, position-x, position-y", ShorthandType::FallThrough);

	RegisterShorthand("decorator", "shape?, color-stops#", ShorthandType::RecursiveCommaSeparated);
}

DecoratorRadialGradientInstancer::~DecoratorRadialGradientInstancer() {}

SharedPtr<Decorator> DecoratorRadialGradientInstancer::InstanceDecorator(const String& name, const PropertyDictionary& properties_,
	const DecoratorInstancerInterface& /*interface_*/)
{
	const Property* p_ending_shape = properties_.GetProperty(ids.ending_shape);
	const Property* p_size_x = properties_.GetProperty(ids.size_x);
	const Property* p_size_y = properties_.GetProperty(ids.size_y);
	const Property* p_position_x = properties_.GetProperty(ids.position_x);
	const Property* p_position_y = properties_.GetProperty(ids.position_y);
	const Property* p_color_stop_list = properties_.GetProperty(ids.color_stop_list);

	if (!p_ending_shape || !p_size_x || !p_size_y || !p_position_x || !p_position_y || !p_color_stop_list)
		return nullptr;

	using namespace RadialGradient;

	Shape shape = (Shape)p_ending_shape->Get<int>();
	if (shape == Shape::Unspecified)
	{
		const bool circle_sized = (Any(p_size_x->unit & Unit::LENGTH) && p_size_y->unit == Unit::KEYWORD);
		shape = (circle_sized ? Shape::Circle : Shape::Ellipse);
	}

	Size size = {};
	if (p_size_x->unit == Unit::KEYWORD)
	{
		size.type = (SizeType)p_size_x->Get<int>();
	}
	else
	{
		size.type = SizeType::LengthPercentage;
		size.x = p_size_x->GetNumericValue();
		size.y = (p_size_y->unit == Unit::KEYWORD ? size.x : p_size_y->GetNumericValue());
	}

	Position position = {};
	for (int dimension = 0; dimension < 2; dimension++)
	{
		NumericValue& value = (dimension == 0 ? position.x : position.y);
		const Property& property = (dimension == 0 ? *p_position_x : *p_position_y);
		if (property.unit == Unit::KEYWORD)
		{
			enum { TOP_LEFT, CENTER, BOTTOM_RIGHT };
			switch (property.Get<int>())
			{
			case TOP_LEFT: value = NumericValue(0.f, Unit::PERCENT); break;
			case CENTER: value = NumericValue(50.f, Unit::PERCENT); break;
			case BOTTOM_RIGHT: value = NumericValue(100.f, Unit::PERCENT); break;
			}
		}
		else
		{
			value = property.GetNumericValue();
		}
	}

	if (p_color_stop_list->unit != Unit::COLORSTOPLIST)
		return nullptr;

	const bool repeating = (name == "repeating-radial-gradient");
	const ColorStopList& color_stop_list = p_color_stop_list->value.GetReference<ColorStopList>();

	auto decorator = MakeShared<DecoratorRadialGradient>();
	if (decorator->Initialise(repeating, shape, size, position, color_stop_list))
		return decorator;

	return nullptr;
}
} // namespace Rml
