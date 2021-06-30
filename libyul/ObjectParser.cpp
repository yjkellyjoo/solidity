/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * Parser for Yul code and data object container.
 */

#include <libyul/AST.h>
#include <libyul/ObjectParser.h>

#include <libyul/AsmParser.h>
#include <libyul/Exceptions.h>

#include <liblangutil/Token.h>

#include <regex>

using namespace std;
using namespace solidity;
using namespace solidity::yul;
using namespace solidity::util;
using namespace solidity::langutil;

namespace
{

// NB: also in AsmParser, so maybe worth adding to libsolutil?
optional<unsigned> toUnsignedInt(string const& _value)
{
	try
	{
		auto const ulong = stoul(_value);
		if (ulong > std::numeric_limits<unsigned>::max())
			return nullopt;
		return static_cast<unsigned>(ulong);
	}
	catch (...)
	{
		return nullopt;
	}
}

}

shared_ptr<Object> ObjectParser::parse(shared_ptr<Scanner> const& _scanner, bool _reuseScanner)
{
	m_recursionDepth = 0;
	try
	{
		shared_ptr<Object> object;
		m_scanner = _scanner;
		if (currentToken() == Token::LBrace)
		{
			// Special case: Code-only form.
			object = make_shared<Object>();
			object->name = "object"_yulstring;
			object->code = parseBlock();
			if (!object->code)
				return nullptr;
		}
		else
			object = parseObject();
		if (object && !_reuseScanner)
			expectToken(Token::EOS);
		return object;
	}
	catch (FatalError const&)
	{
		if (m_errorReporter.errors().empty())
			throw; // Something is weird here, rather throw again.
	}
	return nullptr;
}

shared_ptr<Object> ObjectParser::parseObject(Object* _containingObject)
{
	RecursionGuard guard(*this);

	if (currentToken() != Token::Identifier || currentLiteral() != "object")
		fatalParserError(4294_error, "Expected keyword \"object\".");
	advance();

	shared_ptr<Object> ret = make_shared<Object>();
	ret->name = parseUniqueName(_containingObject);

	expectToken(Token::LBrace);

	ret->code = parseCode();

	while (currentToken() != Token::RBrace)
	{
		if (currentToken() == Token::Identifier && currentLiteral() == "object")
			parseObject(ret.get());
		else if (currentToken() == Token::Identifier && currentLiteral() == "data")
			parseData(*ret);
		else
			fatalParserError(8143_error, "Expected keyword \"data\" or \"object\" or \"}\".");
	}
	if (_containingObject)
		addNamedSubObject(*_containingObject, ret->name, ret);

	expectToken(Token::RBrace);

	return ret;
}

shared_ptr<Block> ObjectParser::parseCode()
{
	if (currentToken() != Token::Identifier || currentLiteral() != "code")
		fatalParserError(4846_error, "Expected keyword \"code\".");
	advance();

	return parseBlock();
}

optional<ObjectParser::ReverseSourceNameMap> ObjectParser::tryGetSourceLocationMapping() const
{
	return tryGetSourceLocationMapping(m_scanner->currentCommentLiteral());
}

optional<ObjectParser::ReverseSourceNameMap> ObjectParser::tryGetSourceLocationMapping(std::string const& _text)
{
	// @use-src 0:"abc.sol" , "1:foo.sol" ,2:"bar.sol"
	//
	// UseSrcList := UseSrc (',' UseSrc)*
	// UseSrc     := [0-9]+ ':' FileName
	// FileName   := "(([^\"]|\.)*)"

	// Matches some "@use-src TEXT".
	static std::regex const lineRE = std::regex(
		R"~~~((^|\s+)@use-src\s+(.*)$)~~~",
		std::regex_constants::ECMAScript | std::regex_constants::optimize
	);
	string_view text(_text);
	std::cmatch cm;
	if (!std::regex_search(text.data(), text.data() + text.size(), cm, lineRE))
		return nullopt;
	solAssert(cm.size() == 3, "");

	// Let @c text point to the parameter value (last match).
	text = string_view(cm[2].first, static_cast<size_t>(std::distance(cm[2].first, cm[2].second)));

	// iteratively match for NUM : STRING_LITERAL and increment
	static regex const firstParamRE(R"~~~(\s*(\d+)\s*:\s*"((?:\\\"|[^\"])*)")~~~",
		std::regex_constants::ECMAScript | std::regex_constants::optimize
	);
	static regex const continuationParamRE(R"~~~(\s*,\s*(\d+)\s*:\s*"((?:\\\"|[^\"])*)")~~~",
		std::regex_constants::ECMAScript | std::regex_constants::optimize
	);

	ReverseSourceNameMap result;

	int k = 0;
	while (!text.empty())
	{
		if (!std::regex_search(text.data(), text.data() + _text.size(), cm, k ? continuationParamRE : firstParamRE))
			return nullopt;
		++k;
		solAssert(cm.size() == 3, "");

		auto const len = cm[0].length();
		solAssert(len > 0, "");
		text.remove_prefix(static_cast<size_t>(len));
		solAssert(k <= 16, ""); // A sanity-check to avoid abuse. Should never happen.

		auto const sourceIndex = toUnsignedInt(cm[1].str());
		if (!sourceIndex)
		{
			// TODO: report error
			return nullopt;
		}

		auto fileName = cm[2].str();
		result[*sourceIndex] = fileName;
	}

	return result;
}

optional<ObjectParser::CharStreamMap>
ObjectParser::convertToCharStreamMap(ReverseSourceNameMap const& _reverseSourceNames) const
{
	(void) _reverseSourceNames; // TODO map file names to their CharStream (needs CompilerStack for that)
	return nullopt;
}

shared_ptr<Block> ObjectParser::parseBlock()
{
	// TODO: maybe here check for @use-src?
	unique_ptr<Parser> parser;
	if (auto sourceLocationMap = tryGetSourceLocationMapping())
	{
		auto charStreamMap = convertToCharStreamMap(*sourceLocationMap);
		yulAssert(charStreamMap, "");
		parser = make_unique<Parser>(m_errorReporter, m_dialect, *charStreamMap);
	}
	else
		parser = make_unique<Parser>(m_errorReporter, m_dialect);
	shared_ptr<Block> block = parser->parse(m_scanner, true);
	yulAssert(block || m_errorReporter.hasErrors(), "Invalid block but no error!");
	return block;
}

void ObjectParser::parseData(Object& _containingObject)
{
	yulAssert(
		currentToken() == Token::Identifier && currentLiteral() == "data",
		"parseData called on wrong input."
	);
	advance();

	YulString name = parseUniqueName(&_containingObject);

	if (currentToken() == Token::HexStringLiteral)
		expectToken(Token::HexStringLiteral, false);
	else
		expectToken(Token::StringLiteral, false);
	addNamedSubObject(_containingObject, name, make_shared<Data>(name, asBytes(currentLiteral())));
	advance();
}

YulString ObjectParser::parseUniqueName(Object const* _containingObject)
{
	expectToken(Token::StringLiteral, false);
	YulString name{currentLiteral()};
	if (name.empty())
		parserError(3287_error, "Object name cannot be empty.");
	else if (_containingObject && _containingObject->name == name)
		parserError(8311_error, "Object name cannot be the same as the name of the containing object.");
	else if (_containingObject && _containingObject->subIndexByName.count(name))
		parserError(8794_error, "Object name \"" + name.str() + "\" already exists inside the containing object.");
	advance();
	return name;
}

void ObjectParser::addNamedSubObject(Object& _container, YulString _name, shared_ptr<ObjectNode> _subObject)
{
	_container.subIndexByName[_name] = _container.subObjects.size();
	_container.subObjects.emplace_back(std::move(_subObject));
}
