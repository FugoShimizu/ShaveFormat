#include "GrammarProfile.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/CharClass.hpp"
#include "GrammarNames.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
	/**
	 * CSS の空白の前後の除去関数
	 * @param Text 読み取る字面
	 * @return 前後の CSS の空白を除いた字面
	 */
	std::string_view TrimCss(std::string_view Text) {
		// CSS の空白の字
		constexpr std::string_view Spaces = " \t\n\r\f";
		Text.remove_prefix(std::min(Text.find_first_not_of(Spaces), Text.size()));
		Text.remove_suffix(Text.size() - std::min(Text.find_last_not_of(Spaces) + 1, Text.size()));
		// 前後の空白を除いた字面の返戻
		return Text;
	}

	/**
	 * CSS の空白の指定の値が空白又は改行を保持するかの判定関数
	 * @param Property 小文字へ揃えた宣言の名前
	 * @param Value 小文字へ揃えた宣言の値
	 * @return 保持する値又は未確定の値なら true
	 */
	bool KeepsCssWhitespace(const std::string_view Property, const std::string_view Value) {
		// 空白の畳み方だけを指定する宣言の返戻
		if(Property == "white-space-collapse") return Value != "collapse" && Value != "discard";
		// 単独で保持を表す値の返戻
		if(Value == "pre" || Value == "pre-wrap" || Value == "pre-line") return true;
		bool Keeps = false;
		// 一括指定の語毎の判定
		for(size_t Start = 0; Start != std::string_view::npos;) {
			const size_t End = std::min(Value.find_first_of(" \t\n\r\f", Start), Value.size());
			const std::string_view Token = Value.substr(Start, End - Start);
			if(Token == "preserve" || Token == "preserve-breaks" || Token == "preserve-spaces" || Token == "break-spaces") Keeps = true;
			// 未知の語（確定出来ない値）の返戻
			else if(
				Token != "normal" && Token != "nowrap" && Token != "wrap" && Token != "collapse" && Token != "discard" && Token != "auto" &&
				Token != "balance" && Token != "stable" && Token != "pretty" && Token != "suppress" && Token != "discard-before" &&
				Token != "discard-after" && Token != "discard-inner"
			) return true;
			Start = Value.find_first_not_of(" \t\n\r\f", End);
		}
		// 保持を表す語の有無の返戻
		return Keeps;
	}

	/**
	 * style 属性の空白保持の指定の判定関数
	 * 計算量：属性値の長さ L に対し O(L)
	 * @param Style 属性値
	 * @return 有効な宣言が空白又は改行を保持するか，値を確定出来なければ true
	 */
	bool PreservesStyleWhitespace(const std::string_view Style) {
		// コメントを CSS の空白へ換え，引用符と逃避の内部を保った字面
		std::string Clean;
		Clean.reserve(Style.size());
		char Quote = '\0';
		for(size_t Pos = 0; Pos < Style.size(); ++Pos) {
			const char Char = Style[Pos];
			if(Quote) {
				Clean += Char;
				if(Char == '\\' && Pos + 1 < Style.size()) Clean += Style[++Pos];
				else if(Char == Quote) Quote = '\0';
			} else if(Char == '/' && Pos + 1 < Style.size() && Style[Pos + 1] == '*') {
				const size_t End = Style.find("*/", Pos + 2);
				// 閉じないコメント（確定出来ない指定）の返戻
				if(End == std::string_view::npos) return true;
				Clean += ' ';
				Pos = End + 1;
			} else {
				if(Char == '\'' || Char == '"') Quote = Char;
				Clean += Char;
			}
		}
		// 閉じない引用符・規則の塊（確定出来ない指定）の返戻
		if(Quote || Clean.find_first_of("{}") != std::string::npos) return true;
		// 宣言の名前と値は大小文字を問わない
		ToLowerAscii(Clean);
		bool HasValue = false, IsImportant = false, Preserves = false;
		// 宣言毎の走査（後の同順位の宣言が勝ち，重要宣言は通常の宣言より優先する）
		for(size_t Start = 0; Start < Clean.size();) {
			size_t End = Start, Colon = std::string::npos;
			int Depth = 0;
			// 逃避・引用符・括弧の外の `:` と宣言の終わりの `;` の探索
			for(; End < Clean.size() && (Depth || Quote || Clean[End] != ';'); ++End) {
				const char Char = Clean[End];
				if(Char == '\\' && End + 1 < Clean.size()) ++End;
				else if(Quote) Quote = Char == Quote ? '\0' : Quote;
				else if(Char == '\'' || Char == '"') Quote = Char;
				else if(Char == '(') ++Depth;
				else if(Char == ')') {
					// 対応しない閉じ括弧（確定出来ない指定）の返戻
					if(!Depth) return true;
					--Depth;
				} else if(!Depth && Char == ':' && Colon == std::string::npos) Colon = End;
			}
			// 閉じない引用符・括弧（確定出来ない指定）の返戻
			if(Quote || Depth) return true;
			const std::string_view Property =
			TrimCss(std::string_view(Clean).substr(Start, Colon == std::string::npos ? 0 : Colon - Start));
			// 逃避・入子の名前（確定出来ない指定）の返戻
			if(Property.find_first_of("\\&") != std::string_view::npos) return true;
			if(Colon != std::string::npos && (Property == "white-space" || Property == "white-space-collapse")) {
				std::string_view Value = TrimCss(std::string_view(Clean).substr(Colon + 1, End - Colon - 1));
				const size_t Bang = Value.rfind('!');
				const bool IsImportantValue = Bang != std::string_view::npos && TrimCss(Value.substr(Bang + 1)) == "important";
				if(IsImportantValue) Value = TrimCss(Value.substr(0, Bang));
				// 空でない値で，先の重要宣言に負けない宣言の採用
				if(!Value.empty() && (!HasValue || IsImportantValue || !IsImportant)) {
					HasValue = true;
					IsImportant = IsImportantValue;
					Preserves = KeepsCssWhitespace(Property, Value);
				}
			}
			Start = End + 1;
		}
		// 最後に有効な空白の指定の返戻
		return Preserves;
	}
} // namespace

/**
 * 字句の間の行内空白の判定関数
 * @param Point 符号位置
 * @return 文法が空白として読めば true
 */
bool ReadingRules::IsSpace(const uint32_t Point) const {
	// 全ての文法が空白と読む ASCII の字の返戻
	if(Point == ' ' || Point == '\t' || Point == '\v' || Point == '\f') return true;
	// BOM を空白と読むかの返戻
	if(Point == 0XFEFF) return ReadsByteOrderMarkAsSpace;
	// Zs の字を空白と読むかの返戻
	return ReadsUnicodeSpaces &&
	(Point == 0XA0 || Point == 0X1680 || Point > 0X1FFF && Point < 0X200B || Point == 0X202F || Point == 0X205F || Point == 0X3000);
}

/**
 * 字句間の空白に JSX 本文固有の U+0085 と U+200B を加えた行内空白の判定関数
 * @param Point 符号位置
 * @return JSX の行端で落とす空白なら true
 */
bool ReadingRules::IsJsxSpace(const uint32_t Point) const {
	// 字句間の空白又は JSX 本文固有の空白の返戻
	return IsSpace(Point) || Point == 0X85 || Point == 0X200B;
}

/**
 * 文字列の開きの前の接頭辞の取得関数（整形仕様 4.2）
 * @param Language 対象言語
 * @return 引用符の前に置いて文字列の種類を定める接頭辞（無ければ空）
 */
std::span<const std::string_view> GrammarProfile::StringPrefixes(const Lang Language) {
	// 文字列接頭辞の文法別の定義
	static constexpr std::string_view CFamily[] = { "L", "LR", "R", "U", "UR", "u", "u8", "u8R", "uR" };
	static constexpr std::string_view CSharp[] = { "$", "$$", "$@", "@", "@$" };
	static constexpr std::string_view Rust[] = { "b", "br", "c", "cr", "r" };
	static constexpr std::string_view Python[] = {
		"B",
		"BR",
		"Br",
		"F",
		"FR",
		"Fr",
		"R",
		"RB",
		"RF",
		"RT",
		"Rb",
		"Rf",
		"Rt",
		"T",
		"TR",
		"Tr",
		"U",
		"b",
		"bR",
		"br",
		"f",
		"fR",
		"fr",
		"r",
		"rB",
		"rF",
		"rT",
		"rb",
		"rf",
		"rt",
		"t",
		"tR",
		"tr",
		"u"
	};
	// C 系の接頭辞の返戻
	if(Language.IsCFamily()) return CFamily;
	// C# の接頭辞の返戻
	if(Language == Lang::CSharp) return CSharp;
	// Rust の接頭辞の返戻
	if(Language == Lang::Rust) return Rust;
	// Python の接頭辞と，接頭辞を持たない言語の返戻
	return Language == Lang::Python ? std::span<const std::string_view>(Python) : std::span<const std::string_view>();
}

/**
 * 節点名の役割の取得関数（整形仕様 4.3，字句間空白仕様 6.1）
 * 同じ役割を持つ複数の節点名を共通の役割へ投影し，同じ節点名が文法毎に異なる構文を表す物は其の文法だけに役割を付ける
 * @param Language 対象言語
 * @param Name 文法の節点名
 * @param IsNamed 構文の節点か（偽は字句）
 * @return 役割の集合
 */
Core::RoleSet GrammarProfile::DeriveRoles(const Lang Language, const std::string_view Name, const bool IsNamed) {
	using Core::Role;
	Core::RoleSet Roles;
	const auto Add = [&Roles](const Role Added) -> void {
		Roles.set(static_cast<size_t>(Added));
	};
	const auto Has = [&Roles](const Role Wanted) -> bool {
		// 付与済の役割の返戻
		return Roles.test(static_cast<size_t>(Wanted));
	};
	// 正規化の規則・制御の流れ・署名・コメント
	if(Name == "mixin_statement") Add(Role::MixinStatement);
	if(Name == "color_value") Add(Role::ColorValue);
	if(Name == "arguments") Add(Role::CallArguments);
	if(Language == Lang::Ruby && Name == "<<") Add(Role::HeredocAmbiguousShift);
	if(Name == "declaration") Add(Role::TypedDeclaration);
	if(Name == "string") Add(Role::QuotedString);
	if(Name == "module") Add(Role::Module);
	if(Name == "block") Add(Role::LexicalBlock);
	if(Name == "jsx_attribute") Add(Role::UnescapedAttribute);
	if(Name == "for_statement") Add(Role::CountedLoop);
	if(Name == "identifier") Add(Role::Identifier);
	if(Names::NameToken.Contains(Name)) Add(Role::NameToken);
	if(Name == "binary_expression") Add(Role::BinaryOperation);
	if(Name == "function_call_expression") Add(Role::FunctionCallExpression);
	if(Name == "attribute") Add(Role::Attribute);
	if(Name == "object_creation_expression") Add(Role::ObjectCreationExpression);
	if(Name == "parenthesized_expression") Add(Role::ValueParentheses);
	if(Name == "exit_statement") Add(Role::ExitStatement);
	if(Name == "lambda_expression") Add(Role::LambdaExpression);
	if(Name == "parameter_list") Add(Role::DeclaratorParameters);
	if(Name == "parameter_declaration") Add(Role::ParameterDeclaration);
	if(Name == "primitive_type") Add(Role::PrimitiveType);
	if(Name == "inferred_parameters") Add(Role::InferredParameters);
	if(Name == "parameter") Add(Role::Parameter);
	if(Name == "modifier") Add(Role::Modifier);
	if(Name == "lambda_function_type") Add(Role::LambdaFunctionType);
	if(Name == "lambda_parameter") Add(Role::LambdaParameter);
	if(Name == "simple_identifier") Add(Role::SimpleName);
	if(Name == "call_suffix") Add(Role::CallSuffix);
	if(Name == "value_arguments") Add(Role::ValueArguments);
	if(Name == "call_expression") Add(Role::CallExpression);
	if(Name == "lambda_literal") Add(Role::LambdaLiteral);
	if(Name == "annotation") Add(Role::Annotation);
	if(Name == "type_modifiers") Add(Role::TypeModifiers);
	if(Name == "enum_entry") Add(Role::EnumEntry);
	if(Name == "class_declaration") Add(Role::ClassDeclaration);
	if(Name == "namespace_definition") Add(Role::NamespaceDefinition);
	if(Name == "namespace_alias_definition") Add(Role::NamespaceAliasDefinition);
	if(Name == "alias_declaration") Add(Role::AliasDeclaration);
	if(Name == "enum_specifier") Add(Role::EnumSpecifier);
	if(Name == "type_definition") Add(Role::TypeDefinition);
	if(Name == "sized_type_specifier") Add(Role::SizedTypeSpecifier);
	if(Name == "type_identifier") Add(Role::TypeName);
	if(Name == "qualified_identifier") Add(Role::QualifiedIdentifier);
	if(Name == "pointer_declarator") Add(Role::PointerDeclarator);
	if(Name == "array_declarator") Add(Role::ArrayDeclarator);
	if(Name == "function_declarator") Add(Role::FunctionDeclarator);
	if(Name == "compound_statement") Add(Role::CompoundBody);
	if(Name == "for_range_loop") Add(Role::ForRangeLoop);
	if(Name == "function_definition") Add(Role::FunctionDefinition);
	if(Name == "statements") Add(Role::ValueStatementSequence);
	if(Name == "postfix_unary_expression") Add(Role::PostfixUnaryExpression);
	if(Name == "postfix_expression") Add(Role::PostfixExpression);
	if(Name == "integer") Add(Role::IntegerValue);
	if(Name == "while_expression") Add(Role::WhileExpression);
	if(Name == "boolean_literal") Add(Role::BooleanLiteral);
	if(Name == "while") Add(Role::KeywordWhile);
	if(Name == "number_literal") Add(Role::NumericToken);
	if(Name == "empty_statement") Add(Role::EmptyStatement);
	if(Name == "true") Add(Role::TrueValue);
	if(Name == "field_identifier") Add(Role::FieldName);
	if(Name == "init_declarator") Add(Role::InitDeclarator);
	if(Name == "type_qualifier") Add(Role::TypeQualifier);
	if(Name == "range") Add(Role::Range);
	if(Name == "assignment") Add(Role::AssignmentExpression);
	if(Name == "in") Add(Role::PatternMembership);
	if(Name == "argument_list") Add(Role::ArgumentSequence);
	if(Name == "method_parameters") Add(Role::MethodParameters);
	if(Name == "lambda_parameters") Add(Role::LambdaParameters);
	if(Name == "call") Add(Role::ReceiverCall);
	if(Name == "constant") Add(Role::ConstantName);
	if(Name == "yield") Add(Role::YieldValue);
	if(Name == "subscript") Add(Role::Subscript);
	if(Name == "type_parameter") Add(Role::GenericParameter);
	if(Name == "generic_type") Add(Role::AppliedGenericType);
	if(Name == "expression_list") Add(Role::ExpressionList);
	if(Name == "pattern_list") Add(Role::PatternList);
	if(Name == "tuple_expression") Add(Role::TupleExpression);
	if(Name == "enum_body_declarations") Add(Role::EnumBodyDeclarations);
	if(Name == "statement_list") Add(Role::PlainStatementSequence);
	if(Name == "expression_case") Add(Role::ExpressionCase);
	if(Names::DefaultCase.Contains(Name)) Add(Role::DefaultCase);
	if(Name == "type_case") Add(Role::TypeCase);
	if(Name == "communication_case") Add(Role::CommunicationCase);
	if(Name == "ambient_declaration") Add(Role::AmbientDeclaration);
	if(Name == "function_expression") Add(Role::FunctionExpression);
	if(Name == "class") Add(Role::Class);
	if(Name == "return") Add(Role::ReturnKeyword);
	if(Name == "binary") Add(Role::Binary);
	if(Name == "unary") Add(Role::UnaryValue);
	if(Name == "element_reference") Add(Role::ElementReference);
	if(Name == "begin") Add(Role::ExplicitKeywordBody);
	if(Name == "operator_assignment") Add(Role::CompoundAssignment);
	if(Name == "assignment_statement") Add(Role::ValueDiscardingAssignment);
	if(Name == "regex") Add(Role::Regex);
	if(Name == "heredoc_beginning") Add(Role::HeredocBeginning);
	if(Name == "foreach_statement") Add(Role::ForeachStatement);
	if(Names::ScssMixinRule.Contains(Name)) Add(Role::MixinRule);
	if(Names::CssSelectorHost.Contains(Name)) Add(Role::SelectorHost);
	if(Names::PythonDocstringOwner.Contains(Name)) Add(Role::LeadingStringDocumentOwner);
	if(Names::StmtCtxParent.Contains(Name)) Add(Role::UnusedValueHost);
	if(Names::GroupingParen.Contains(Name)) Add(Role::GroupingParen);
	if(Names::PostfixReceiverHost.Contains(Name)) Add(Role::PostfixExpressionHost);
	if(Names::ConstructionReceiverHost.Contains(Name)) Add(Role::ConstructionReceiverHost);
	if(Names::JavaEmptyArgumentHost.Contains(Name)) Add(Role::OptionalAnnotationArguments);
	if(Names::FunctionLikeAny.Contains(Name)) Add(Role::FunctionScope);
	if(Names::ClassBody.Contains(Name)) Add(Role::ClassBody);
	if(Names::JavaStatementHost.Contains(Name)) Add(Role::DiscardedBranchValueHost);
	if(Names::NumberLiteral.Contains(Name)) Add(Role::NumberLiteral);
	if(Names::CppDeclTypeSpecifier.Contains(Name)) Add(Role::DeclarationTypeSpecifier);
	if(Names::TupleLike.Contains(Name)) Add(Role::TupleLike);
	if(Names::PhpAltCloser.Contains(Name)) Add(Role::AlternativeBodyCloser);
	if(Name == "anonymous_class") Add(Role::AnonymousClass);
	if(Name == "lambda_function_type_parameters") Add(Role::LambdaFunctionTypeParameters);
	if(Name == "constructor_invocation") Add(Role::ConstructorInvocation);
	if(Name == "attribute_argument_list") Add(Role::AttributeArgumentList);
	if(Name == "initializer_expression") Add(Role::InitializerExpression);
	if(Name == "annotated_lambda") Add(Role::AnnotatedLambda);
	if(Name == "class_body") Add(Role::ClassMembers);
	if(Name == "enum_class_body") Add(Role::EnumClassBody);
	if(Name == "secondary_constructor") Add(Role::SecondaryConstructor);
	if(Name == "primary_constructor") Add(Role::PrimaryConstructor);
	if(Name == "goto_statement") Add(Role::GotoStatement);
	if(Name == "switch_statement") Add(Role::SwitchStatement);
	if(Name == "labeled_statement") Add(Role::LabeledStatement);
	if(Name == "named_label_statement") Add(Role::NamedLabelStatement);
	if(Name == "boolean") Add(Role::Boolean);
	if(Name == "loop_expression") Add(Role::LoopExpression);
	if(Name == "macro_invocation") Add(Role::MacroCall);
	if(Name == "method_invocation") Add(Role::MethodInvocation);
	if(Name == "switch_label") Add(Role::SwitchLabel);
	if(Name == "fallthrough_statement") Add(Role::FallthroughStatement);
	if(Name == "else") Add(Role::Else);
	if(Name == "statement_label") {
		Add(Role::StatementLabel);
		// ラベルは `:` で終わり，後の文へ続く
		Add(Role::ExpressionContinuationTail);
	}
	if(Name == "switch_entry") Add(Role::SwitchEntry);
	if(Name == "preproc_else") Add(Role::PreprocElse);
	if(Name == "ellipsis") Add(Role::Ellipsis);
	if(Name == "translation_unit") Add(Role::TranslationUnit);
	if(Name == "linkage_specification") Add(Role::LinkageSpecification);
	if(Name == "declaration_list") Add(Role::DeclarationList);
	if(Name == "parenthesized_declarator") Add(Role::ParenthesizedDeclarator);
	if(Name == "operator_cast") Add(Role::OperatorCast);
	if(Name == "placeholder_type_specifier") Add(Role::PlaceholderTypeSpecifier);
	if(Name == "trailing_return_type") Add(Role::TrailingReturnType);
	if(Name == "destructor_declaration") Add(Role::DestructorDeclaration);
	if(Language == Lang::Cpp && Name == "destructor_name") Add(Role::MemberDestructorName);
	if(Names::ResultlessRoutine.Contains(Name)) Add(Role::ResultlessRoutine);
	if(Name == "getter" || Name == "computed_getter") Add(Role::ReaderRoutine);
	if(Name == "accessor_declaration") Add(Role::NamedAccessor);
	if(Name == "generic_name") Add(Role::GenericName);
	if(Name == "method_declaration") Add(Role::MethodDeclaration);
	if(Name == "function_value_parameters") Add(Role::FunctionValueParameters);
	if(Name == "function_body") Add(Role::FunctionBody);
	if(Name == "asserts_annotation") Add(Role::AssertsAnnotation);
	if(Name == "body_statement") Add(Role::ImplicitResultBody);
	if(Name == "variable_declarator") Add(Role::VariableDeclaratorItem);
	if(Name == "variable_declaration") Add(Role::VariableDeclaration);
	if(Name == "reference_expression") Add(Role::ReferenceExpression);
	if(Name == "unary_expression") Add(Role::UnaryOperation);
	if(Name == "label") Add(Role::JumpLabel);
	if(Name == "if_expression") Add(Role::ConditionalValue);
	if(Name == "for_expression") Add(Role::IterationValue);
	if(Name == "match_block") Add(Role::MatchBlock);
	if(Names::ScopeBody.Contains(Name)) Add(Role::ScopeBody);
	if(IsNamed && Names::FlowReturn.Contains(Name)) Add(Role::FlowReturn);
	if(Names::KeywordJump.Contains(Name)) Add(Role::KeywordJump);
	if(Names::Comment.Contains(Name)) Add(Role::Comment);
	if(Names::LoopExitStatement.Contains(Name)) Add(Role::LoopExitStatement);
	if(Name == "volatile") Add(Role::ObservableQualifier);
	if(Names::ContinueKeyword.Contains(Name)) Add(Role::ContinueKeyword);
	if(Names::BreakKeyword.Contains(Name)) Add(Role::BreakKeyword);
	if(Names::IntLiteralLike.Contains(Name)) Add(Role::IntegerLiteralCandidate);
	if(Names::CondParenWrap.Contains(Name)) Add(Role::CondParenWrap);
	if(Names::WhileLoop.Contains(Name)) Add(Role::WhileLoop);
	if(Names::CallNode.Contains(Name)) Add(Role::CallNode);
	if(Names::SwitchBody.Contains(Name)) Add(Role::SwitchBody);
	if(IsNamed && Names::CaseClause.Contains(Name)) Add(Role::CaseClause);
	if(Names::CaseBodyOpener.Contains(Name)) Add(Role::CaseBodyOpener);
	if(Names::ConditionalBranchClause.Contains(Name)) Add(Role::ConditionalBranchClause);
	if(Names::ElseAny.Contains(Name)) Add(Role::ElseAny);
	if(Names::CatchClause.Contains(Name)) Add(Role::CatchClause);
	if(Names::FinallyClause.Contains(Name)) Add(Role::FinallyClause);
	if(IsNamed && Names::ReturnOrThrow.Contains(Name)) Add(Role::ReturnOrThrow);
	if(Names::StatementFlowContainer.Contains(Name)) Add(Role::StatementFlowContainer);
	if(Names::SingleBodyStatement.Contains(Name)) Add(Role::SingleBodyStatement);
	if(Names::TryStatement.Contains(Name)) Add(Role::TryStatement);
	if(Names::FunctionBodyContainer.Contains(Name)) Add(Role::FunctionBodyContainer);
	if(Names::PythonPlaceholder.Contains(Name)) Add(Role::PlaceholderStatement);
	if(Names::CDeclarationSpecifier.Contains(Name)) Add(Role::DeclarationSpecifier);
	if(Names::CFunctionOrFieldDeclarator.Contains(Name)) Add(Role::DeclaratorWrapper);
	if(Names::PointerOrRefDeclarator.Contains(Name)) Add(Role::PointerOrRefDeclarator);
	if(Names::KotlinReturnType.Contains(Name)) Add(Role::PostParameterResultType);
	if(Names::VariableDeclarator.Contains(Name)) Add(Role::VariableDeclarator);
	if(Names::NonConstantExpression.Contains(Name)) Add(Role::NonConstantExpression);
	if(Names::QualifiedMember.Contains(Name)) Add(Role::QualifiedMember);
	if(Names::ThisReference.Contains(Name)) Add(Role::ThisReference);
	if(Names::RustDeclarationStatement.Contains(Name)) Add(Role::NonValueDeclaration);
	if(Names::RustPlaceProjection.Contains(Name)) Add(Role::PlaceProjection);
	if(Names::RustScrutineeHost.Contains(Name)) Add(Role::ScrutineeHost);
	if(Names::RustBorrowingOperator.Contains(Name)) Add(Role::BorrowingOperator);
	if(Names::RustBodyOnlyLoop.Contains(Name)) Add(Role::BodyTemporaryScopeLoop);
	if(Names::RustLetCondition.Contains(Name)) Add(Role::PatternCondition);
	if(Names::LazyLogicalOperator.Contains(Name)) Add(Role::LazyLogicalOperator);
	if(Names::RustPlaceOrPlainValue.Contains(Name)) Add(Role::PlaceOrPlainValue);
	if(Name == "for_clause") Add(Role::ForClause);
	if(Name == "range_clause") Add(Role::RangeClause);
	if(Name == "method_definition") Add(Role::MethodDefinition);
	if(Name == "object") Add(Role::ObjectValue);
	if(Name == "self") Add(Role::SelfValue);
	if(Name == "pointer_type_declarator") Add(Role::PointerTypeDeclarator);
	if(Name == "type_parameters") Add(Role::GenericParameters);
	if(Names::DocWrapper.Contains(Name)) Add(Role::DocWrapper);
	if(Names::AttachPrefix.Contains(Name)) Add(Role::AttachPrefix);
	if(Names::DocumentedDefinition.Contains(Name)) Add(Role::DocumentedDefinition);
	// 文書を付けれる宣言（同じ節点名が文法毎に異なる構文を表す物は宣言を表す文法だけに付け，CSS・SCSS の性質の宣言と Python の根を除く）
	if(
		Names::DocumentableDeclaration.Contains(Name) && !(Language == Lang::CSS && Name == "declaration") &&
		!(Language == Lang::Python && Name == "module") || Language == Lang::Ruby && (Name == "assignment" || Name == "call") ||
		Language == Lang::TypeScript && Name == "property_identifier"
	) Add(Role::DocumentableDeclaration);
	if(Names::DocHostSequence.Contains(Name)) Add(Role::DocHostSequence);
	if(Names::ParamContainerLike.Contains(Name)) Add(Role::ParamContainerLike);
	if(Names::DestructurePattern.Contains(Name)) Add(Role::DestructurePattern);
	if(Names::DeclTypeChild.Contains(Name)) Add(Role::DeclTypeChild);
	if(Names::DefinitionBody.Contains(Name)) Add(Role::DefinitionBody);
	if(Names::SectionKeyword.Contains(Name)) Add(Role::SectionKeyword);
	if(Names::CloserToken.Contains(Name)) Add(Role::CloserToken);
	if(Names::StringLikeInnerPreserve.Contains(Name)) Add(Role::LiteralInterior);
	if(Language == Lang::Ruby && Names::RubyArrayLiteral.Contains(Name)) Add(Role::ArrayValue);
	if(Names::OpaqueLineText.Contains(Name)) Add(Role::OpaqueLineText);
	if(Names::GeneratorMarker.Contains(Name)) Add(Role::GeneratorMarker);
	if(Names::ExitKeyword.Contains(Name)) Add(Role::ExitKeyword);
	if(Language == Lang::Ruby && Name == "body_statement") Add(Role::HoistedDocumentBody);
	if(Language == Lang::Ruby && Has(Role::DocumentedDefinition) || Names::KotlinBraceBodyHost.Contains(Name)) {
		Add(Role::InlineDefinitionBody);
	}
	if((Language == Lang::Kotlin || Language == Lang::Swift) && Has(Role::DocumentedDefinition)) Add(Role::SeparateParameterParts);
	if(Language.IsCFamily() && Has(Role::DocumentedDefinition)) Add(Role::DeclaratorSignature);
	if(Language == Lang::Swift && Has(Role::DocumentedDefinition)) Add(Role::DirectParameterSignature);
	if(Language == Lang::Rust && Name == "parameter") Add(Role::PatternParameter);
	if(
		Name == (
			Language == Lang::Kotlin || Language == Lang::Swift ?
			"simple_identifier" :
			Language == Lang::PHP ? "variable_name" : "identifier"
		)
	) Add(Role::ParameterName);
	if(Language == Lang::Go && Name == "source_file") Add(Role::AdjacentDocumentSequence);
	// 静的検査
	if(Names::AccessOrderMember.Contains(Name)) Add(Role::AccessOrderMember);
	if(Names::AssignmentExpression.Contains(Name)) Add(Role::AssignmentOperation);
	if(Names::AutoParameterHost.Contains(Name)) Add(Role::AutoParameterHost);
	if(Names::CmpBoundaryNode.Contains(Name)) Add(Role::BoundaryComparison);
	if(Names::ConditionFieldHost.Contains(Name)) Add(Role::ConditionFieldHost);
	if(Names::ConditionalAlternative.Contains(Name)) Add(Role::ConditionalAlternative);
	if(Names::CppBuiltinType.Contains(Name)) Add(Role::BuiltinNumericType);
	if(Names::CppConstLiteral.Contains(Name)) Add(Role::ConstantLiteral);
	if(Names::CppUnnamableInitializer.Contains(Name)) Add(Role::UnnameableInitializer);
	if(Names::DeclInitDeclarator.Contains(Name)) Add(Role::DeclInitDeclarator);
	if(Names::DeclLike.Contains(Name)) Add(Role::DeclLike);
	if(Names::FlowExit.Contains(Name)) Add(Role::FlowExit);
	if(Names::FunctionLikeDefinition.Contains(Name)) Add(Role::NamedFunctionDefinition);
	if(Names::FunctionNameIdentifier.Contains(Name)) Add(Role::FunctionNameIdentifier);
	if(Language == Lang::Cpp && Name == "field_identifier") Add(Role::FunctionNameIdentifier);
	if(Names::IdentifierLeafLike.Contains(Name)) Add(Role::IdentifierLeafLike);
	if(Names::IfNode.Contains(Name)) Add(Role::Conditional);
	if(Names::JsxElement.Contains(Name)) Add(Role::TagElement);
	if(Names::JumpWithLabel.Contains(Name)) Add(Role::JumpWithLabel);
	if(Names::KotlinAssignableWrapper.Contains(Name)) Add(Role::AssignableWrapper);
	if(Names::LabeledStatement.Contains(Name)) Add(Role::LabeledConstruct);
	if(Names::Loop.Contains(Name)) Add(Role::RepeatedControl);
	if(Names::MemberFieldRank.Contains(Name)) Add(Role::MemberFieldRank);
	if(Language == Lang::Cpp && Name == "friend_declaration") Add(Role::MemberFriendRank);
	if(Names::MemberMethodRank.Contains(Name)) Add(Role::MemberMethodRank);
	if(Language == Lang::Cpp && Name == "storage_class_specifier") Add(Role::MemberStorageSpecifier);
	if(Names::MemberTypeRank.Contains(Name)) Add(Role::MemberTypeRank);
	if(Names::MutatingExpression.Contains(Name)) Add(Role::MutatingExpression);
	if(Names::NameDeclarator.Contains(Name)) Add(Role::NameDeclarator);
	if(Names::NamedParameterKind.Contains(Name)) Add(Role::NamedParameterKind);
	if(Names::OrdinaryFunction.Contains(Name)) Add(Role::OrdinaryFunctionDefinition);
	if(Names::PlainVariable.Contains(Name)) Add(Role::PlainVariable);
	if(Names::ScopeContainer.Contains(Name)) Add(Role::ScopeContainer);
	if(Names::SimpleTypeLeaf.Contains(Name)) Add(Role::SimpleTypeLeaf);
	if(Names::StringLikeAll.Contains(Name)) Add(Role::StringLikeAll);
	if(Names::SwitchGotoTarget.Contains(Name)) Add(Role::SwitchGotoTarget);
	if(Names::TypeNameNode.Contains(Name)) Add(Role::TypeNameNode);
	if(Names::TypedDeclaration.Contains(Name)) Add(Role::DeclarationWithType);
	if(Names::UnaryPre.Contains(Name)) Add(Role::PrefixUnary);
	if(Names::UnaryPrefixExpression.Contains(Name)) Add(Role::UnaryPrefixExpression);
	if(Names::VariableDeclaration.Contains(Name)) Add(Role::VariableDeclarationStatement);
	if(Name == "access_specifier") Add(Role::AccessSection);
	if(Name == "concatenated_string") Add(Role::ConcatenatedLiteral);
	if(Name == "control_transfer_statement") Add(Role::ControlTransfer);
	if(Name == "custom_operator") Add(Role::UserDefinedOperator);
	if(Name == "else_clause") Add(Role::AlternativeClause);
	if(Name == "enum_body") Add(Role::EnumerationBody);
	if(Name == "exceptions") Add(Role::ExceptionTypes);
	if(Name == "field_declaration") Add(Role::FieldDefinition);
	if(Name == "init_statement") Add(Role::ConditionInitializer);
	if(Name == "jsx_expression") Add(Role::EmbeddedExpression);
	if(Name == "jump_expression") Add(Role::JumpExpression);
	if(Name == "method") Add(Role::InstanceMethodDefinition);
	if(Name == "modifiers") Add(Role::ModifierSequence);
	if(Name == "number") Add(Role::NumericLiteral);
	if(Name == "rescue_modifier") Add(Role::RescueExpression);
	if(Name == "return_statement") Add(Role::ReturnStatement);
	if(Name == "singleton_method") Add(Role::SingletonMethod);
	if(Name == "throw_expression") Add(Role::ThrowValue);
	if(Name == "throw_keyword") Add(Role::ThrowKeyword);
	if(Name == "throw_statement") Add(Role::ThrowStatement);
	if(Name == "throws") Add(Role::ThrowsDeclaration);
	if(Name == "user_type") Add(Role::UserType);
	if(
		Has(Role::FunctionScope) || Has(Role::OrdinaryFunctionDefinition) || Name == "lambda_literal" && Language != Lang::Kotlin ||
		Has(Role::ClassBody) || Name == "async_block"
	) Add(Role::FlowBoundary);
	const bool IsSwitchStatementLike = Names::SwitchStatementLike.Contains(Name);
	if(IsSwitchStatementLike || Language == Lang::Java && Name == "switch_expression") Add(Role::ArmSwitch);
	if(Language == Lang::CSharp && Name == "goto_statement") Add(Role::CaseLabelJump);
	if(Language == Lang::Ruby && Name == "return") Add(Role::ArgumentReturn);
	if(Language == Lang::Rust && Has(Role::LoopExitStatement)) Add(Role::LabeledValueExit);
	const bool IsLoopOrSwitch = Names::LoopOrSwitch.Contains(Name);
	if(
		IsLoopOrSwitch && !(Language == Lang::Swift && Name == "do_statement") || Language == Lang::Java && Name == "switch_expression"
	) Add(Role::BreakTarget);
	if(Language == Lang::PHP && Name == "switch_statement") Add(Role::ContinueExitsSwitch);
	if(Language == Lang::Kotlin && Name == "annotation") Add(Role::TransparentLabelPrefix);
	if(Language == Lang::Go && Name == "for_statement") Add(Role::ClauseOrConditionLoop);
	if(
		(Language.IsCFamily() || Language.IsJsTs() || Language == Lang::CSharp || Language == Lang::Java || Language == Lang::PHP) &&
		Name == "for_statement"
	) Add(Role::OmittableConditionLoop);
	if((Name == "do_statement" || Name == "do_while_statement") && Language != Lang::Swift || Name == "repeat_while_statement") {
		Add(Role::PostConditionLoop);
	}
	if(Language == Lang::Rust && Name == "macro_invocation") Add(Role::TerminatingMacroCall);
	if((Language == Lang::Go || Language == Lang::Kotlin || Language == Lang::Swift) && IsSwitchStatementLike) {
		Add(Role::DirectSwitchArms);
	}
	if(
		(Language == Lang::Rust || Language == Lang::PHP) && Name == "match_expression" ||
		Language == Lang::Swift && Name == "switch_statement"
	) Add(Role::ExhaustiveSwitch);
	if(Language == Lang::Swift && Name == "fallthrough") Add(Role::BareFallthrough);
	if(Language == Lang::Swift && (Name == "if_statement" || Name == "if_expression")) Add(Role::SiblingLabeledBranch);
	if(Language == Lang::Swift && Name == "simple_identifier") Add(Role::FallthroughName);
	if(Language == Lang::Python && IsLoopOrSwitch) Add(Role::LoopElse);
	if(Language == Lang::Rust && Has(Role::StatementFlowContainer)) Add(Role::LabeledBlockExit);
	if(Language == Lang::Swift && Name == "do_statement") Add(Role::CatchableBody);
	if(Language != Lang::Ruby && Has(Role::OrdinaryFunctionDefinition)) Add(Role::ExplicitResultFunction);
	if(Language.IsCFamily() && Has(Role::OrdinaryFunctionDefinition)) Add(Role::CompoundFunctionBody);
	if(Name == (Language == Lang::Go ? "statement_list" : "statements")) Add(Role::InnerFunctionStatements);
	if(Language.IsJsTs() && (Has(Role::OrdinaryFunctionDefinition) || Has(Role::FunctionScope))) Add(Role::StarGeneratorFunction);
	if(Language == Lang::Python && Name == "block") Add(Role::PlaceholderBody);
	if(Language == Lang::Python && Has(Role::OrdinaryFunctionDefinition)) Add(Role::MayDeclarePlaceholderFunction);
	if(Language == Lang::Swift && Has(Role::OrdinaryFunctionDefinition)) Add(Role::SingleExpressionResultFunction);
	if(Language == Lang::Swift && Name == "function_declaration") Add(Role::ImplicitReturnCandidate);
	if(Language == Lang::Swift && Name == "class_declaration") Add(Role::ImplicitReturnNominal);
	if(Language == Lang::Swift && Name == "inheritance_specifier") Add(Role::ImplicitReturnConformance);
	if(Language == Lang::Swift && Name == "extension") Add(Role::ImplicitReturnExtensionToken);
	if(Language == Lang::Swift) {
		if(Name.ends_with("_expression") || Name.ends_with("_literal") || Name == "if_statement" || Name == "switch_statement") {
			Add(Role::ImplicitReturnCandidateExpression);
		}
		static constexpr std::string_view ImplicitReturnExpressions[] {
			"additive_expression",
			"array_literal",
			"as_expression",
			"bitwise_operation",
			"boolean_literal",
			"comparison_expression",
			"conjunction_expression",
			"dictionary_literal",
			"disjunction_expression",
			"equality_expression",
			"infix_expression",
			"integer_literal",
			"line_string_literal",
			"multi_line_string_literal",
			"multiplicative_expression",
			"navigation_expression",
			"nil_coalescing_expression",
			"nil_literal",
			"postfix_expression",
			"prefix_expression",
			"range_expression",
			"real_literal",
			"simple_identifier",
			"ternary_expression",
			"tuple_expression"
		};
		if(std::ranges::find(ImplicitReturnExpressions, Name) != std::end(ImplicitReturnExpressions)) {
			Add(Role::ImplicitReturnCandidateExpression);
			Add(Role::ImplicitReturnExpression);
		}
	}
	if(Name == "parameters") Add(Role::MixinParameters);
	if(Language == Lang::Python && Name == "string") Add(Role::DocumentString);
	if(Language == Lang::Python && Name == "class_definition") Add(Role::OptionalEmptySuperclass);
	if(
		Language == Lang::PHP &&
		(Name == "function_call_expression" || Name == "attribute" || Name == "object_creation_expression" || Name == "exit_statement")
	) Add(Role::OptionalConstructionArguments);
	if(Language == Lang::Cpp && Name == "lambda_expression") Add(Role::OptionalLambdaDeclarator);
	if(Language == Lang::Cpp && !Language.IsCShared && Name == "parameter_list") Add(Role::OptionalVoidParameter);
	if(Language != Lang::Go && Names::PostfixOrUpdateExpression.Contains(Name)) Add(Role::PrefixableIncrement);
	if(Language == Lang::Cpp && Has(Role::PrefixableIncrement)) Add(Role::OverloadableIncrement);
	if(Language == Lang::Java && Name == "switch_rule") Add(Role::ValueSwitchRule);
	if(Language == Lang::Python && Name == "while_statement") Add(Role::CapitalBooleanLoop);
	if(Language == Lang::Rust && Name == "while_expression") Add(Role::UnconditionalLoopKeyword);
	if(
		(Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::Java || Language.IsJsTs() || Language == Lang::PHP) &&
		IsLoopOrSwitch
	) Add(Role::ParenthesizedLoopCondition);
	if(Language.IsCFamily() && Names::BoolInitContext.Contains(Name)) Add(Role::BooleanInitializerDeclaration);
	if(Language.IsCFamily() && Name == "declaration") Add(Role::LeadingConstDeclaration);
	if(Language != Lang::HTML && Language != Lang::CSS && IsNamed && Has(Role::NumberLiteral)) Add(Role::NormalizedNumber);
	if(Language == Lang::CSS && (Has(Role::MixinRule) || Name == "color_value" || Name == "declaration")) Add(Role::StyleLexeme);
	if(
		(Language.IsJsTs() || Language == Lang::PHP || Language == Lang::Ruby || Language == Lang::Python || Language == Lang::CSS) &&
		IsNamed && Has(Role::StringLikeAll)
	) Add(Role::NormalizedQuote);
	if(
		Language == Lang::Ruby &&
		(Name == "while" || Name == "range" || Name == "argument_list" || Name == "method_parameters" || Name == "lambda_parameters")
	) Add(Role::BareArgumentNotation);
	if(Language == Lang::Python && (Name == "subscript" || Name == "type_parameter")) Add(Role::TupleTypeComma);
	if(
		Language == Lang::Python &&
		(Name == "expression_list" || Name == "pattern_list" || Name == "lambda_parameters" || Name == "tuple_expression")
	) Add(Role::BareListTrailingComma);
	if(
		Language == Lang::Go && (Name == "source_file" || Names::StatementList.Contains(Name) || Names::GoMemberList.Contains(Name)) ||
		Language == Lang::Python && (Name == "module" || Name == "block") || Language == Lang::Swift && Name == "enum_entry"
	) Add(Role::OptionalSemicolonHost);
	if(Language == Lang::Go && Name == "statement_list") Add(Role::EmptySemicolonSequence);
	if(Language == Lang::Rust && Has(Role::OrdinaryFunctionDefinition)) Add(Role::AttributedTailFunction);
	if(
		Language == Lang::PHP &&
		(IsLoopOrSwitch || Name == "if_statement" || Name == "declare_statement" || Name == "switch_block" || Name == "colon_block")
	) Add(Role::OptionalLoopTerminator);
	// 中間表現の構築と配置
	if(Name == "text") Add(Core::Role::LiteralText);
	if(Name == "%w(" || Name == "%i(") Add(Core::Role::WordArrayOpen);
	if(Name == ")") Add(Core::Role::WordArrayClose);
	if(Language.IsJsTs() && (Name == "if_statement" || Name == "else_clause")) Add(Core::Role::DependentBranchLayout);
	if(Names::BreakOpaque.Contains(Name)) Add(Core::Role::BreakOpaque);
	if(Names::CompactRecurse.Contains(Name)) Add(Core::Role::CompactRecurse);
	if(Names::Compact.Contains(Name)) Add(Core::Role::Compact);
	if(Names::StringSeqContainer.Contains(Name)) Add(Core::Role::StringSeqContainer);
	if(Names::RubyArrayLiteral.Contains(Name)) Add(Core::Role::WordArray);
	if(Names::JsxTag.Contains(Name)) Add(Core::Role::ExpressionTag);
	if(Names::OptionalTypeMember.Contains(Name)) Add(Core::Role::OptionalTypeMember);
	if(Names::BreakColonHost.Contains(Name)) Add(Core::Role::BreakColonHost);
	if(Names::InfixOp.Contains(Name)) Add(Core::Role::InfixOp);
	if(Names::InitHeader.Contains(Name)) Add(Core::Role::InitHeader);
	if(Names::PipeParamList.Contains(Name)) Add(Core::Role::PipeParamList);
	if(Name == "super_expression") Add(Core::Role::BaseReference);
	if(Name == "typed_parameter") Add(Core::Role::AnnotatedParameter);
	if(Name == "typed_default_parameter") Add(Core::Role::AnnotatedDefaultParameter);
	if(Name == "union_type") Add(Core::Role::UnionType);
	if(Name == "type") Add(Core::Role::TypeSlot);
	if(Name == "optional_type") Add(Core::Role::OptionalType);
	if(Name == "lambda_capture_specifier") Add(Core::Role::CaptureList);
	if(Name == "type_arguments") Add(Core::Role::GenericArguments);
	if(Names::StringInterpolation.Contains(Name)) Add(Core::Role::StringInterpolation);
	if(Names::ElseLikeClause.Contains(Name)) Add(Core::Role::ElseLikeClause);
	if(Name == "guard_statement") Add(Core::Role::GuardStatement);
	if(Names::PreprocConditional.Contains(Name)) Add(Core::Role::PreprocConditional);
	if(Name == "php_tag") Add(Core::Role::CodeOpeningTag);
	if(Names::ExpandContainer.Contains(Name)) Add(Core::Role::ExpandContainer);
	if(Names::AngleBracketList.Contains(Name)) Add(Core::Role::AngleBracketList);
	if(Names::NamespaceLike.Contains(Name)) Add(Core::Role::NamespaceLike);
	if(Names::Transparent.Contains(Name)) Add(Core::Role::Transparent);
	if(Names::AmbiguousBodyContainer.Contains(Name)) Add(Core::Role::AmbiguousBodyContainer);
	if(Language == Lang::Ruby && Name == "heredoc_body") Add(Core::Role::DeferredLiteralBody);
	if(Name == "import_spec_list") Add(Core::Role::ImportItems);
	if(Name == "import_spec") Add(Core::Role::ImportItem);
	if(Name == "do") Add(Core::Role::IterationSection);
	if(Names::ObjectLike.Contains(Name)) Add(Core::Role::ObjectLike);
	if(Names::RubyClassDefLike.Contains(Name)) Add(Core::Role::InheritedClass);
	if(Names::IndentContainer.Contains(Name)) Add(Core::Role::IndentContainer);
	if(Names::StatementList.Contains(Name)) Add(Core::Role::StatementList);
	if(Names::GoGroupedDecl.Contains(Name)) Add(Core::Role::GroupedDeclaration);
	if(Names::PhpAltHost.Contains(Name)) Add(Core::Role::ColonBodyHost);
	if(Names::JsxContainer.Contains(Name)) Add(Core::Role::RenderedChildren);
	if(Names::StatementsOnlyBreak.Contains(Name)) Add(Core::Role::StatementsOnlyBreak);
	if(Names::RubyMethodDef.Contains(Name)) Add(Core::Role::EndDelimitedMethod);
	if(Names::UnaryPreOrUpdate.Contains(Name)) Add(Core::Role::UnaryPreOrUpdate);
	if(Names::JsxTextLike.Contains(Name)) Add(Core::Role::RenderedText);
	if(Name == "array") Add(Core::Role::ArrayLiteral);
	if(Name == "decorator") Add(Core::Role::DeclarationDecorator);
	if(Name == "text_interpolation") Add(Core::Role::CodeTextBoundary);
	if(Names::RangeOperator.Contains(Name)) Add(Core::Role::RangeOperator);
	if(Names::ParenLeadType.Contains(Name)) Add(Core::Role::ParenLeadType);
	if(Names::PostfixReceiverChain.Contains(Name)) Add(Core::Role::PostfixReceiverChain);
	if(Names::RubyNumericLiteralSuffix.Contains(Name)) Add(Core::Role::NumericUnitSuffix);
	if(Names::MisparsedTemplateHost.Contains(Name)) Add(Core::Role::MisparsedTemplateHost);
	if(Names::OptionalMarker.Contains(Name)) Add(Core::Role::OptionalMarker);
	if(Names::AttachColonOrDot.Contains(Name)) Add(Core::Role::AttachColonOrDot);
	if(Names::SpaceSkip.Contains(Name)) Add(Core::Role::SpaceSkip);
	if(Names::NormColon.Contains(Name)) Add(Core::Role::NormColon);
	if(Names::TypeCombination.Contains(Name)) Add(Core::Role::TypeCombination);
	if(Names::CombinatorSelector.Contains(Name)) Add(Core::Role::CombinatorSelector);
	if(Names::CssNumericValue.Contains(Name)) Add(Core::Role::DimensionNumber);
	if(Names::KotlinNotIsIn.Contains(Name)) Add(Core::Role::NegatableTestWord);
	if(Names::GeneratorStarHost.Contains(Name)) Add(Core::Role::GeneratorStarHost);
	if(Names::NewParenCall.Contains(Name)) Add(Core::Role::NewParenCall);
	if(Names::CastExpr.Contains(Name)) Add(Core::Role::CastExpr);
	if(Names::CssSelectorName.Contains(Name)) Add(Core::Role::SelectorName);
	if(Names::MacroLike.Contains(Name)) Add(Core::Role::MacroLike);
	if(Names::AttachBracket.Contains(Name)) Add(Core::Role::AttachBracket);
	if(Names::LeadingTypeArguments.Contains(Name)) Add(Core::Role::LeadingTypeArguments);
	if(Names::AttachParen.Contains(Name)) Add(Core::Role::AttachParen);
	if(Names::ReturnTuple.Contains(Name)) Add(Core::Role::ReturnTuple);
	if(Names::Control.Contains(Name)) Add(Core::Role::Control);
	if(Names::CParenDeclarator.Contains(Name)) Add(Core::Role::CompoundDeclarator);
	if(Names::BracketType.Contains(Name)) Add(Core::Role::BracketType);
	if(Names::SwiftKeywordNode.Contains(Name)) Add(Core::Role::NamedKeyword);
	if(Names::BraceAttach.Contains(Name)) Add(Core::Role::BraceAttach);
	if(Names::PointerDecl.Contains(Name)) Add(Core::Role::PointerDecl);
	if(Names::OpenTailToken.Contains(Name)) Add(Core::Role::OpenTailToken);
	if(Has(Role::PrefixableIncrement) || Language == Lang::Go && Names::PostfixOrUpdateExpression.Contains(Name)) {
		Add(Core::Role::PostfixOrUpdateExpression);
	}
	if(Names::ColonPairHost.Contains(Name)) Add(Core::Role::ColonPairHost);
	if(Names::RangeLike.Contains(Name)) Add(Core::Role::RangeLike);
	if(Names::CCastTypeNameInner.Contains(Name)) Add(Core::Role::CastTypeAtom);
	if(Names::UnaryPrefixToken.Contains(Name)) Add(Core::Role::UnaryPrefixToken);
	if(Names::SeparatorToken.Contains(Name)) Add(Core::Role::SeparatorToken);
	if(Names::ParameterContainer.Contains(Name)) Add(Core::Role::ParameterContainer);
	if(Names::HtmlTag.Contains(Name)) Add(Core::Role::TagSyntax);
	if(Names::GoType.Contains(Name)) Add(Core::Role::BracketedType);
	if(Names::SpacelessBrace.Contains(Name)) Add(Core::Role::SpacelessBrace);
	if(Names::ColonControl.Contains(Name)) Add(Core::Role::ColonControl);
	if(Names::Member.Contains(Name)) Add(Core::Role::Member);
	if(Names::BraceNoSpace.Contains(Name)) Add(Core::Role::BraceNoSpace);
	if(Names::ControlKeyword.Contains(Name)) Add(Core::Role::ControlKeyword);
	if(Names::PatternKind.Contains(Name)) Add(Core::Role::PatternKind);
	if(Names::RubyFlowExpr.Contains(Name)) Add(Core::Role::ValueJump);
	if(Names::SpaceKeyword.Contains(Name)) Add(Core::Role::SpaceKeyword);
	if(Names::ArrowToken.Contains(Name)) Add(Core::Role::ArrowToken);
	if(Names::KotlinFunctionHead.Contains(Name)) Add(Core::Role::GenericFunctionHead);
	if(Name == "conditional") Add(Core::Role::ConditionalOperator);
	if(Name == "interpolation") Add(Core::Role::Interpolation);
	if(Name == "template_function") Add(Core::Role::GenericFunction);
	if(Name == "binary_operator") Add(Core::Role::BinaryOperator);
	if(Name == "plain_value") Add(Core::Role::UnitText);
	if(Name == "variadic_type_parameter_declaration") Add(Core::Role::VariadicTypeParameter);
	if(Name == "function_type") Add(Core::Role::FunctionType);
	if(Name == "spread_parameter") Add(Core::Role::SpreadParameter);
	if(Name == "parameter_pack_expansion" || Name == "base_class_clause" || Name == "lambda_capture_initializer") {
		Add(Core::Role::PackExpansion);
	}
	if(Name == "sizeof_expression") Add(Core::Role::SizeofExpression);
	if(Name == "this_expression") Add(Core::Role::SelfReference);
	if(Name == "indexer_declaration") Add(Core::Role::Indexer);
	if(Name == "bracketed_parameter_list") Add(Core::Role::IndexerParameters);
	if(Name == "implicit_array_creation_expression") Add(Core::Role::InferredArrayConstruction);
	if(Name == "type_annotation") Add(Core::Role::TypeAnnotation);
	if(Name == "attribute_declaration") Add(Core::Role::AttributeDeclaration);
	if(Name == "template_string") Add(Core::Role::InterpolatedLiteral);
	if(Name == "template_substitution") Add(Core::Role::TemplateInterpolation);
	if(Name == "interpolation_alignment_clause") Add(Core::Role::InterpolationAlignment);
	if(Name == "block_argument") Add(Core::Role::BlockArgument);
	if(Name == "class_selector") Add(Core::Role::ClassSelector);
	if(Name == "nesting_selector") Add(Core::Role::NestingSelector);
	if(Name == "id_selector") Add(Core::Role::IdSelector);
	if(Name == "attribute_selector") Add(Core::Role::AttributeSelector);
	if(Name == "optional_chain") Add(Core::Role::OptionalChain);
	if(Name == "bang") Add(Core::Role::ExclamationMark);
	if(Name == "except_clause") Add(Core::Role::ExceptionClause);
	if(Name == "pointer_type") Add(Core::Role::PointerType);
	if(Name == "value_argument") Add(Core::Role::ValueArgument);
	if(Name == "operator_name") Add(Core::Role::OperatorName);
	if(Name == "channel_type") Add(Core::Role::ChannelType);
	if(Name == "key_path_expression") Add(Core::Role::KeyPath);
	if(Name == "reference_assignment_expression") Add(Core::Role::ReferenceAssignment);
	if(Name == "field_declaration_list") Add(Core::Role::FieldList);
	if(Name == "export_statement") Add(Core::Role::ExportDeclaration);
	if(Names::JsxHtmlTagOpenClose.Contains(Name)) Add(Core::Role::ContentTagBoundary);
	if(Name == "jsx_text") Add(Core::Role::ElementText);
	if(Name == "when_entry") Add(Core::Role::ValueArm);
	if(Name == "block_body") Add(Core::Role::ClosureBody);
	if(Name == "enumerator_list") Add(Core::Role::EnumeratorSequence);
	if(Language == Lang::Ruby && Name == "heredoc_beginning") Add(Core::Role::DeferredLiteralStart);
	if(Names::PhpAltLeadingColon.Contains(Name)) Add(Core::Role::LeadingColonBody);
	if(Name == "colon_block") Add(Core::Role::ColonBody);
	if(Names::PreprocDirectiveOpen.Contains(Name)) Add(Core::Role::PreprocDirectiveOpen);
	if(Names::PreprocDirectiveClose.Contains(Name)) Add(Core::Role::PreprocDirectiveClose);
	if(IsSwitchStatementLike) Add(Core::Role::SwitchStatementLike);
	if(Name == "switch_expression") Add(Core::Role::SwitchValue);
	if(Name == "in_clause") Add(Core::Role::PatternSection);
	if(Names::ClassBodyContainer.Contains(Name)) Add(Core::Role::ClassBodyContainer);
	if(Names::ClassLike.Contains(Name)) Add(Core::Role::ClassLike);
	if(Language.IsJsTs() && (Name == "identifier" || Name == "await")) Add(Core::Role::AwaitCallHead);
	if(Language == Lang::PHP && Has(Role::AlternativeBodyCloser)) Add(Core::Role::ColonBodyEnd);
	if(Names::CloserWord.Contains(Name)) Add(Core::Role::CloserWord);
	if(Language == Lang::Swift && Has(Role::InfixOp)) Add(Core::Role::AdjacencySensitiveInfix);
	if(Language == Lang::Ruby && Names::RubyStatementSequence.Contains(Name)) Add(Core::Role::DelimitedStatementSequence);
	if(Language == Lang::Swift && Name == "directive") Add(Core::Role::HardLineDirective);
	if(Language == Lang::Ruby && Name == "exception_variable") Add(Core::Role::UnbrokenExceptionBinding);
	if(Language == Lang::Swift && Names::SwiftPostfixCall.Contains(Name)) Add(Core::Role::OptionalInvocation);
	if(Language == Lang::PHP && Names::PhpUnsplittable.Contains(Name)) Add(Core::Role::UnbrokenParenSyntax);
	if(Language == Lang::Swift && Name == "import_declaration") Add(Core::Role::UnbrokenImport);
	if(Language == Lang::Kotlin && Name == "super_expression") Add(Core::Role::UnbrokenBaseReference);
	if(Language == Lang::Swift && Name == "optional_type") Add(Core::Role::UnbrokenOptionalType);
	if(Language == Lang::Go && Names::GoMemberList.Contains(Name)) Add(Core::Role::DelimitedMemberSequence);
	if(Language == Lang::Ruby && Names::RubyConditionHost.Contains(Name)) Add(Core::Role::EmptyConditionBody);
	if(Language == Lang::Kotlin && Name == "for_statement") Add(Core::Role::EmptyLoopBody);
	if(Language == Lang::Go && !Names::MultilineCommaContainer.Contains(Name)) Add(Core::Role::NoDelimiterBreak);
	if(Language == Lang::TypeScript && Name == "binary_expression") Add(Core::Role::GenericLookalikeOperator);
	if(Language == Lang::Ruby && Name == "block") Add(Core::Role::ClosureBlock);
	if(Language == Lang::PHP && Names::PhpOpenTagEnd.Contains(Name)) Add(Core::Role::EmbeddedOpeningBoundary);
	if(Language == Lang::Ruby && Name == "chained_string") Add(Core::Role::UnbrokenStringSequence);
	if(Language == Lang::Ruby && Has(Role::ExpandContainer)) Add(Core::Role::KeywordExpandedContainer);
	if(Language == Lang::Go && Has(Role::AngleBracketList)) Add(Core::Role::ExpandedTypeParameters);
	if(Language == Lang::Kotlin && Names::KotlinAccessor.Contains(Name)) Add(Core::Role::AttachedAccessor);
	if(Language == Lang::Go && Name == "import_declaration") Add(Core::Role::GroupedImport);
	if(
		Language.IsCFamily() &&
		(Has(Role::PreprocConditional) || Name == "preproc_elif" || Name == "preproc_elifdef" || Name == "preproc_else")
	) Add(Core::Role::DirectiveBlankBoundary);
	if(Language == Lang::Swift && Name == "switch_statement") Add(Core::Role::SwitchBodyLayout);
	if(Language == Lang::Kotlin && Has(Role::InlineDefinitionBody)) Add(Core::Role::DirectBraceLayout);
	if(Language == Lang::Swift && Names::SwiftBraceBodyHost.Contains(Name)) Add(Core::Role::DirectControlBraceLayout);
	if(Language == Lang::PHP && Has(Role::ColonBodyHost)) Add(Core::Role::AlternateBodyLayout);
	if(Language == Lang::Go && Names::GoSwitch.Contains(Name)) Add(Core::Role::TypedSwitchLayout);
	if(Language == Lang::Rust && Name == "empty_statement") Add(Core::Role::AttachedEmptyStatement);
	if(Language != Lang::Ruby && Has(Role::InheritedClass)) Add(Core::Role::NonKeywordClass);
	if(Language == Lang::CSharp && Name == "implicit_type") Add(Core::Role::DeconstructionType);
	if(Language == Lang::Python && Name == "binary_operator") Add(Core::Role::AnnotationUnionOperator);
	if(Language == Lang::Ruby && Has(Role::NumericUnitSuffix)) Add(Core::Role::AttachedNumericUnit);
	if(Language == Lang::Swift && Names::RangeOrUnaryExpression.Contains(Name)) Add(Core::Role::AttachedRangeOrUnary);
	if(Language == Lang::Rust && Name == "assignment_expression") Add(Core::Role::RangeAssignment);
	if(Language == Lang::Rust && Name == "label") Add(Core::Role::AttachedLabel);
	if(Language == Lang::HTML && Name == "start_tag") Add(Core::Role::SpacedAttributeTag);
	if(Language == Lang::CSharp && Name == "lambda_expression") Add(Core::Role::TypedLambda);
	if(Language == Lang::Kotlin && Name == "constructor_invocation") Add(Core::Role::AnnotationConstructor);
	if(Language == Lang::CSharp && Has(Role::PointerDecl)) Add(Core::Role::PostfixPointerType);
	if(Language == Lang::Kotlin && Name == "spread_expression") Add(Core::Role::ArgumentSpread);
	if(Language == Lang::Rust && Name == "range_expression") Add(Core::Role::OpenRangeOperand);
	if(Language == Lang::CSS && Names::CssNamespacePrefix.Contains(Name)) Add(Core::Role::AttachedSelectorNamespace);
	if(Language == Lang::Ruby && Name == "lambda") Add(Core::Role::AttachedArrowLambda);
	if(Language == Lang::Cpp && Name == "if_statement") Add(Core::Role::ConstantConditionHost);
	if(Language == Lang::Swift && Name == "function_declaration") Add(Core::Role::OperatorFunctionDeclaration);
	if(Language.IsCFamily() && Name == "binary_expression") Add(Core::Role::CastLookalikeOperator);
	if(Language == Lang::Ruby && Name == "block_body") Add(Core::Role::ClosureStatements);
	if(Language == Lang::Ruby && Names::RubyBlock.Contains(Name)) Add(Core::Role::KeywordControlLayout);
	if(Language == Lang::Kotlin && Name == "if_expression") Add(Core::Role::BracedConditionalValue);
	if(Language == Lang::Python && (Name == "binary_operator" || Name == "union_type" || Name == "type")) {
		Add(Core::Role::AnnotationUnionHost);
	}
	if(
		Language == Lang::Python && (
			Has(Role::AssignmentOperation) || Name == "typed_parameter" || Name == "typed_default_parameter" || Name == "for_statement" ||
			Name == "union_type" || Name == "type" || Name == "binary_operator" || Name == "type_alias_statement"
		)
	) Add(Core::Role::ContinuationHeader);
	if(Language == Lang::Swift && Names::SwiftHeaderStatement.Contains(Name)) Add(Core::Role::TightHeaderEquals);
	if(Language == Lang::Swift && IsNamed && Name == "parameter") Add(Core::Role::TightParameterEquals);
	if(Language == Lang::Ruby && Names::RubyAnonymousForward.Contains(Name)) Add(Core::Role::TightAnonymousForward);
	if(
		Language == Lang::HTML && (
			Name == "document" || Name == "element" || Name == "script_element" || Name == "style_element" || Has(Role::TagSyntax) ||
			Name == "attribute" || Name == "attribute_name" || Name == "attribute_value" || Name == "quoted_attribute_value" ||
			Name == "text" || Name == "raw_text" || Name == "entity" || Name == "doctype" || Name == "erroneous_end_tag"
		)
	) Add(Core::Role::MarkupSyntax);
	if(Language == Lang::Kotlin && Name == "label") Add(Core::Role::PrefixLabel);
	if(Name == "then") Add(Core::Role::ThenSection);
	if(Name == "ensure") Add(Core::Role::EnsureSection);
	if(Name == "rescue") Add(Core::Role::RescueSection);
	if(Language == Lang::Ruby && (Name == "assignment" || Name == "operator_assignment")) Add(Core::Role::KeywordValueAssignment);
	if(Language == Lang::Ruby && (Name == "binary" || Name == "conditional")) Add(Core::Role::CompactInterpolationOperator);
	if((Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::TypeScript) && Has(Role::MisparsedTemplateHost)) {
		Add(Core::Role::RecoverableTemplate);
	}
	if(Language == Lang::Ruby && Names::HashSplat.Contains(Name)) Add(Core::Role::AttachedHashSplat);
	if(Language == Lang::Ruby && Names::RubyLowUnary.Contains(Name)) Add(Core::Role::AttachedLowUnaryWord);
	if(Language == Lang::PHP && Name == "reference_modifier") Add(Core::Role::AttachedReferenceModifier);
	if(Language == Lang::PHP && Names::PhpNamespaceUse.Contains(Name)) Add(Core::Role::AttachedNamespaceImport);
	if(Language == Lang::Kotlin && Name == "control_structure_body") Add(Core::Role::SpacedControlBody);
	if(Language == Lang::Kotlin && Name == "prefix_expression") Add(Core::Role::LabeledPrefix);
	if(Language == Lang::Kotlin && Names::KotlinNegatableTest.Contains(Name)) Add(Core::Role::AttachedNegationTest);
	if(Language == Lang::Kotlin && Names::KotlinNumericLiteralSuffix.Contains(Name)) Add(Core::Role::AttachedMagnitudeSuffix);
	if(Language == Lang::CSS && Name == "%") Add(Core::Role::AttachedPercent);
	if(Language == Lang::CSS && Names::CssSelectorAtomic.Contains(Name)) Add(Core::Role::AttachedSelector);
	if((Language == Lang::Swift || Language == Lang::TypeScript) && Name == "!") Add(Core::Role::PostfixAssertionMark);
	if(Language == Lang::Ruby && Has(Role::AttachParen)) Add(Core::Role::BareCallArguments);
	if(Language == Lang::Ruby && Has(Role::UnaryPreOrUpdate)) Add(Core::Role::SignedOperandHost);
	if((Language == Lang::Python || Language.IsJsTs()) && Has(Role::NumberLiteral)) Add(Core::Role::IntegerMemberBoundary);
	if(Name == "sizeof" || Language == Lang::CSharp && Name == "typeof") Add(Core::Role::AttachedTypeQuery);
	if(Language != Lang::Swift && Name == "function_declaration") Add(Core::Role::SpacedFunctionHead);
	if(Name == "infix_expression") Add(Core::Role::NamedInfix);
	if(Language == Lang::Ruby && Names::RubyBraceBlock.Contains(Name)) Add(Core::Role::InlineBraceBody);
	if(Has(Role::RepeatedControl) && !(Language == Lang::Swift && Name == "do_statement")) Add(Role::LabeledLoop);
	if(!(Language == Lang::Swift && Name == "do_statement") && (IsLoopOrSwitch || Names::SwitchOrWithExpression.Contains(Name))) {
		Add(Role::NestedBreakTarget);
	}
	if(
		Names::ClassLikeStrict.Contains(Name) || Language == Lang::Java && Name == "enum_declaration" ||
		Language == Lang::Swift && Name == "extension_declaration"
	) Add(Role::AccessOrderedClass);
	if(Language == Lang::Cpp && Name == "class_specifier") Add(Role::ImplicitPrivateClass);
	if(
		Name == (Language == Lang::Java ? "class_body" : "declaration_list") ||
		(Language == Lang::Kotlin || Language == Lang::Swift || Language == Lang::TypeScript) && Name == "class_body"
	) Add(Role::AccessOrderedBody);
	if(
		Name == (Language == Lang::PHP ? "visibility_modifier" : "modifier") || Name == "visibility_modifier" ||
		Name == "accessibility_modifier"
	) Add(Role::AccessModifier);
	if(
		(Language == Lang::Kotlin || Language == Lang::Swift) && (
			Name == "function_declaration" || Name == "property_declaration" || Name == "init_declaration" ||
			Name == "subscript_declaration"
		) || Language == Lang::TypeScript && (Name == "public_field_definition" || Name == "method_definition")
	) Add(Role::AccessOrderMember);
	if(
		(Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::Go || Language == Lang::PHP) && Name == "goto_statement"
	) Add(Role::GotoInspection);
	if(
		(Language == Lang::Java || Language.IsJsTs() || Language == Lang::Go) && Name == "labeled_statement" ||
		(Language == Lang::Rust || Language == Lang::Kotlin) && Name == "label" || Language == Lang::Swift && Name == "statement_label"
	) Add(Role::LabelInspection);
	if(
		Language == Lang::Cpp && Name == "placeholder_type_specifier" || Language == Lang::CSharp && Name == "implicit_type" ||
		Language == Lang::Java && Name == "type_identifier" || Language == Lang::TypeScript && Name == "predefined_type"
	) Add(Role::TypeInferenceInspection);
	if(
		Language == Lang::Python && Name == "function_definition" ||
		(Language == Lang::TypeScript || Language == Lang::Kotlin || Language == Lang::Swift) && Name == "function_declaration" ||
		Language == Lang::Rust && Name == "function_item"
	) Add(Role::ReturnHintRequired);
	if(Language == Lang::TypeScript && Name == "variable_declarator") Add(Role::OptionalDeclarationType);
	if(
		Language == Lang::TypeScript && (Name == "required_parameter" || Name == "optional_parameter") ||
		Language == Lang::Python && (Name == "identifier" || Name == "default_parameter")
	) Add(Role::OptionalParameterType);
	if(Language == Lang::Cpp && Name == "argument_list") Add(Role::MutableArgumentList);
	if(Language.IsCFamily() && Name == "pointer_expression") Add(Role::AddressOperation);
	if(Language == Lang::Cpp && !Language.IsCShared && Name == "declaration") Add(Role::ConstexprInspection);
	if(Language.IsCFamily() && Name == "preproc_def") Add(Role::MacroConstantInspection);
	if(Language.IsCFamily() && Name == "using_declaration") Add(Role::ScopeImport);
	if(Names::CondContextParent.Contains(Name)) Add(Role::ZeroConditionHost);
	if(Name == "binary_expression" || Name == "comparison_operator" || Name == "comparison_expression") {
		Add(Role::ZeroComparisonInspection);
	}
	if(Language.IsJsTs() && Name == "binary_expression") Add(Role::StrictEqualityInspection);
	if(Language == Lang::Cpp && !Language.IsCShared && Name == "cast_expression") Add(Role::CastInspection);
	if(Language == Lang::Cpp && !Language.IsCShared && Names::CppNullCandidate.Contains(Name)) Add(Role::NullPointerInspection);
	if(
		(Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::Java || Language == Lang::PHP || Language.IsJsTs()) &&
		Has(Role::Conditional)
	) Add(Role::ConditionMergeInspection);
	if(Has(Role::AssignmentOperation) || Name == "operator_assignment") Add(Role::BoolFlipInspection);
	if(Has(Role::AssignmentOperation) || Name == "operator_assignment" || Name == "assignment_statement") {
		Add(Role::IncrementInspection);
	}
	if(Has(Role::PostfixOrUpdateExpression)) Add(Role::PostfixInspection);
	if(Name == "number" || Name == "number_literal" || Name == "integer") Add(Role::ZeroLiteral);
	if(
		Language.IsJsTs() ? Name == "member_expression" : Language == Lang::PHP ? Name == "function_call_expression" : Name == "call"
	) Add(Role::CountExpression);
	if(Language == Lang::Kotlin && Name == "modifiers") Add(Role::FailureAnnotation);
	if(Language == Lang::Rust && Has(Role::DocumentedDefinition)) Add(Role::ErrorResult);
	if(Language == Lang::Ruby && Name == "identifier") Add(Role::ImplicitRaiseName);
	if(
		Has(Role::CatchClause) || Has(Role::FinallyClause) ||
		Language == Lang::Ruby && (Name == "rescue" || Name == "ensure" || Name == "else")
	) Add(Role::ExceptionContinuation);
	if(
		Has(Role::TryStatement) || Name == "do_statement" || Language == Lang::Ruby && (Name == "begin" || Name == "body_statement")
	) Add(Role::ExceptionBody);
	if(Has(Role::CatchClause) || Language == Lang::Ruby && Name == "rescue") Add(Role::ExceptionHandler);
	/** ========== 括弧・波括弧・宣言の正規化 ========== */
	if(Names::RubyLowUnary.Contains(Name)) Add(Role::LowPrecedenceUnaryKeyword);
	if(Names::RubyPatternTest.Contains(Name)) Add(Role::LowPrecedencePatternTest);
	if(Names::TypeColonParent.Contains(Name)) Add(Role::TypeColonParent);
	if(Names::OpenPrefixExpression.Contains(Name)) Add(Role::OpenPrefixExpression);
	if(Names::KeywordUnary.Contains(Name)) Add(Role::KeywordUnary);
	if(Names::SwitchOrWithExpression.Contains(Name)) Add(Role::SwitchOrWithExpression);
	if(Names::PythonPostfix.Contains(Name)) Add(Role::CallAttributeOrSubscript);
	if(Names::IterationHeader.Contains(Name)) Add(Role::IterationHeader);
	if(Names::PostfixReceiverLeaf.Contains(Name)) Add(Role::PostfixReceiverLeaf);
	if(Names::PostfixCallHost.Contains(Name)) Add(Role::PostfixCallHost);
	if(Names::GoTypeName.Contains(Name)) Add(Role::CompositeTypeName);
	if(Names::ParenInnerNoUnwrap.Contains(Name)) Add(Role::ParenInnerNoUnwrap);
	if(Names::RubyStatementHost.Contains(Name)) Add(Role::CommandStatementHost);
	if(Names::TypeArgumentLookalikeSlot.Contains(Name)) Add(Role::TypeArgumentLookalikeSlot);
	if(Names::JsInRestoring.Contains(Name)) Add(Role::MembershipExpressionBoundary);
	if(Names::PythonNumber.Contains(Name)) Add(Role::AttributeSensitiveNumber);
	if(Names::ReturnValueParent.Contains(Name)) Add(Role::ReturnValueParent);
	if(Names::ReturnValueHost.Contains(Name)) Add(Role::ReturnValueHost);
	if(Names::ForEachHeader.Contains(Name)) Add(Role::ForEachHeader);
	if(Names::AwaitTryCastExpression.Contains(Name)) Add(Role::AwaitTryCastExpression);
	if(Names::CsPostfixChain.Contains(Name)) Add(Role::ConditionalAccessChain);
	if(Names::CCastInner.Contains(Name)) Add(Role::CastTypePart);
	if(Names::ParenValueSlot.Contains(Name)) Add(Role::ParenValueSlot);
	if(Names::ExprStmtLiteral.Contains(Name)) Add(Role::ExprStmtLiteral);
	if(Names::GreedyTailExpression.Contains(Name)) Add(Role::GreedyTailExpression);
	if(Names::ParenInnerFirstChildNoUnwrap.Contains(Name)) Add(Role::ParenInnerFirstChildNoUnwrap);
	if(Names::RustBlockLike.Contains(Name)) Add(Role::StatementEndingBlock);
	if(Names::RustStatementHead.Contains(Name)) Add(Role::BlockExpressionStatementHost);
	if(Names::RustValueJump.Contains(Name)) Add(Role::OptionalValueJump);
	if(Names::RangeOrUnaryExpression.Contains(Name)) Add(Role::RangeOrUnaryExpression);
	if(Names::TypeCastRight.Contains(Name)) Add(Role::TypeCastRight);
	if(Names::GroupingTypeParen.Contains(Name)) Add(Role::GroupingTypeParen);
	if(Names::TsTypeOperatorHost.Contains(Name)) Add(Role::AlgebraicTypeOperandHost);
	if(Names::TsGreedyType.Contains(Name)) Add(Role::GreedyType);
	if(Names::TsFunctionType.Contains(Name)) Add(Role::CallableType);
	if(Names::SwiftTypeOperatorHost.Contains(Name)) Add(Role::ComposedTypeOperandHost);
	if(Language == Lang::Kotlin && Names::KotlinTypeOperatorHost.Contains(Name)) Add(Role::NullableTypeOperandHost);
	if(Language == Lang::Rust && Names::RustTypeOperatorHost.Contains(Name)) Add(Role::BorrowedTypeOperandHost);
	if(Names::GoConversionType.Contains(Name)) Add(Role::UnambiguousConversionType);
	if(Names::TypeOperatorInner.Contains(Name)) Add(Role::TypeOperatorInner);
	if(Name == "tuple") Add(Role::TupleValue);
	if(Name == "tuple_pattern") Add(Role::TuplePattern);
	if(Name == "case_pattern") Add(Role::CasePattern);
	if(Name == "as_pattern") Add(Role::AliasPattern);
	if(Name == "union_pattern") Add(Role::AlternativePattern);
	if(Name == "list_splat") Add(Role::SequenceExpansion);
	if(Name == "named_expression") Add(Role::NamedValue);
	if(Name == "for_in_clause") Add(Role::ComprehensionIteration);
	if(Name == "delete_statement") Add(Role::DeleteStatement);
	if(Name == "member_expression") Add(Role::MemberExpression);
	if(Name == "prefix_expression") Add(Role::PrefixExpression);
	if(Name == "open_start_range_expression") Add(Role::RangeWithoutStart);
	if(Name == "update_expression") Add(Role::UpdateExpression);
	if(Name == "open_end_range_expression") Add(Role::RangeWithoutEnd);
	if(Name == "invocation_expression") Add(Role::InvocationExpression);
	if(Name == "await") Add(Role::AwaitValue);
	if(Name == "conditional_expression") Add(Role::ConditionalExpression);
	if(Name == "not_operator") Add(Role::LogicalNegation);
	if(Name == "field_expression") Add(Role::FieldExpression);
	if(Name == "navigation_suffix") Add(Role::NavigationSuffix);
	if(Name == "type_cast_expression") Add(Role::TrailingTypeCast);
	if(Name == "cast_expression") Add(Role::CastExpression);
	if(Name == "abstract_pointer_declarator") Add(Role::AbstractPointer);
	if(Name == "abstract_function_declarator") Add(Role::AbstractFunction);
	if(Name == "abstract_array_declarator") Add(Role::AbstractArray);
	if(Name == "if_clause") Add(Role::ComprehensionFilter);
	if(Name == "for_in_statement") Add(Role::ForInStatement);
	if(Name == "decltype") Add(Role::ValueCategoryType);
	if(Name == "type_switch_statement") Add(Role::TypeSwitch);
	if(Name == "conditional_access_expression") Add(Role::ConditionalAccessExpression);
	if(Name == "argument") Add(Role::Argument);
	if(Name == "auto") Add(Role::DeducedTypeKeyword);
	if(Name == "ternary_expression") Add(Role::TernaryExpression);
	if(Name == "fold_expression") Add(Role::FoldExpression);
	if(Name == "range_expression") Add(Role::RangeExpression);
	if(Name == "tuple_type_item") Add(Role::TupleTypeItem);
	if(Name == "type_pack_expansion") Add(Role::TypePackExpansion);
	if(Name == "conditional_type") Add(Role::ConditionalType);
	if(Name == "intersection_type") Add(Role::IntersectionType);
	if(Name == "infer_type") Add(Role::InferredType);
	if(Name == "protocol_composition_type") Add(Role::ProtocolComposition);
	if(Name == "type_conversion_expression") Add(Role::TypeConversion);
	if(Name == "value_binding_pattern") Add(Role::ValueBindingPattern);
	if(Name == "reference_modifier") Add(Role::ReferenceModifier);
	if(Names::DeclaratorHead.Contains(Name)) Add(Role::DeclaratorHead);
	if(Names::JsMemberTarget.Contains(Name)) Add(Role::MemberMutationTarget);
	if(Names::JsBindingName.Contains(Name)) Add(Role::BindingName);
	if(Names::RustBaseTraversal.Contains(Name)) Add(Role::MutationBaseTraversal);
	if(Names::RustRefPattern.Contains(Name)) Add(Role::ReferenceBindingPattern);
	if(Names::JavaTypeBody.Contains(Name)) Add(Role::ParameterScopeBoundary);
	if(Names::JavaParameterScope.Contains(Name)) Add(Role::ParameterScope);
	if(Names::WritableAssignment.Contains(Name)) Add(Role::WritableAssignment);
	if(Name == "attribute_item") Add(Role::OuterExpansionAttribute);
	if(Name == "inner_attribute_item") Add(Role::InnerExpansionAttribute);
	if(Name == "import_statement") Add(Role::ImportStatement);
	if(Name == "ref_pattern") Add(Role::ReferencePattern);
	if(Name == "mut_pattern") Add(Role::MutablePattern);
	if(Name == "compact_constructor_declaration") Add(Role::CompactConstructor);
	if(Name == "record_declaration") Add(Role::RecordDeclaration);
	if(Name == "captured_pattern") Add(Role::CapturedPattern);
	if(Name == "let_condition") Add(Role::BindingCondition);
	if(Name == "closure_expression") Add(Role::ClosureExpression);
	if(Name == "global_statement") Add(Role::GlobalStatement);
	if(Name == "mutable_specifier") Add(Role::MutableSpecifier);
	if(Name == "binding_pattern_kind") Add(Role::BindingMarker);
	if(Language == Lang::Swift && Name == "infix_expression") Add(Role::ComparisonInfix);
	if(Language == Lang::Swift && Name == "prefix_expression") Add(Role::PrefixAbsorbsInfix);
	if(Language == Lang::Swift && Name == "call_expression") Add(Role::OperatorCall);
	if(Language == Lang::Kotlin && Name == "infix_expression") Add(Role::NamedInfixCall);
	if(Language == Lang::Swift && Name == "as_expression") Add(Role::ComposedTypeCast);
	if(Language == Lang::Ruby && Name == "call") Add(Role::CommandCall);
	if((Language == Lang::CSharp || Language == Lang::Java) && Name == "object_creation_expression") Add(Role::AtomicConstruction);
	if((Language == Lang::CSharp || Language == Lang::Kotlin || Language == Lang::Swift) && Has(Role::NumberLiteral)) {
		Add(Role::AtomicNumber);
	}
	if(Language == Lang::Rust && Name == "try_expression") Add(Role::PostfixTry);
	if(Language == Lang::Swift && Name == "?") Add(Role::OptionalNavigationMark);
	if(Language == Lang::Rust && Name == "type_cast_expression") Add(Role::UnaryReceiverCast);
	if(Language == Lang::Rust && Has(Role::AwaitTryCastExpression)) Add(Role::PostfixValueOperation);
	if(Language == Lang::Rust && Name == "await_expression") Add(Role::PostfixAwait);
	if(Language == Lang::CSharp && Name == "switch_expression") Add(Role::UnwrappedSwitchSubject);
	if(Language == Lang::Go && Name == "unary_expression") Add(Role::ChannelReceive);
	if(Language == Lang::Cpp && Names::DecltypeIdExpression.Contains(Name)) Add(Role::DeducedValueCategoryName);
	if(Language == Lang::CSharp && Name == "interpolation") Add(Role::ColonSensitiveInterpolation);
	if(Language == Lang::Python && Name == "conditional_expression") Add(Role::ConditionalTailOnly);
	if(Language == Lang::Ruby && (Name == "unary" || Has(Role::LowPrecedencePatternTest))) Add(Role::LowKeywordPrecedence);
	if((Language.IsJsTs() || Language == Lang::CSharp) && Name == "await_expression") Add(Role::PrefixAwait);
	if(Language.IsJsTs() && Has(Role::PostfixReceiverChain)) Add(Role::AwaitNameReceiver);
	if(Language == Lang::CSharp && (Name == "invocation_expression" || Has(Role::SwitchOrWithExpression))) {
		Add(Role::SwitchWithPrecedence);
	}
	if(
		Language == Lang::Python &&
		(Name == "await" || Has(Role::CallAttributeOrSubscript) || Name == "conditional_expression" || Name == "not_operator")
	) Add(Role::KeywordExpressionPrecedence);
	if(
		Language.IsCFamily() && Name == "identifier" ||
		(Language.IsCFamily() || Language == Lang::Rust || Language == Lang::Kotlin || Language == Lang::PHP) &&
		Names::FieldCallee.Contains(Name) || Language == Lang::Kotlin && Name == "simple_identifier" ||
		Language == Lang::PHP && Name == "name"
	) Add(Role::CalleeNeedsGrouping);
	if(Language.IsJsTs() && Has(Role::GroupingParen)) Add(Role::ConstructionReceiverParen);
	if(Language == Lang::Ruby && Has(Role::GroupingParen)) Add(Role::CommandArgumentParen);
	if(Language == Lang::Python && (Name == "if_clause" || Name == "attribute" || Name == "await")) {
		Add(Role::RestrictedExpressionOperand);
	}
	if(Language == Lang::PHP && Has(Role::GroupingParen)) Add(Role::ReferenceValueParen);
	if(Language == Lang::PHP && Has(Role::ForEachHeader)) Add(Role::DelimitedIterationTarget);
	if(
		(
			Language == Lang::Go || Language == Lang::Rust || Language == Lang::Swift || Language == Lang::Ruby || Language == Lang::Python
		) && Names::ParenOptionalHeader.Contains(Name)
	) Add(Role::OptionalConditionParen);
	if(Language.IsCFamily() && Name == "cast_expression") Add(Role::KeywordCastOperand);
	if(Language == Lang::CSharp && Has(Role::GroupingParen)) Add(Role::ConditionalAccessParen);
	if((Language == Lang::Cpp || Language == Lang::CSharp || Language == Lang::TypeScript) && Has(Role::GroupingParen)) {
		Add(Role::TypeArgumentAmbiguousParen);
	}
	if((Language.IsCFamily() || Language == Lang::CSharp) && Name == "cast_expression") Add(Role::CastBeforeUnary);
	if(Language.IsCFamily() && Has(Role::GroupingParen)) Add(Role::CastAmbiguousParen);
	if(Language != Lang::Swift || Names::SwiftHeaderExpressionHost.Contains(Name)) Add(Role::BodyAmbiguousHeader);
	if(Language == Lang::Rust && Has(Role::GroupingParen)) Add(Role::BlockEndingParen);
	if(Language == Lang::Swift && Has(Role::GroupingParen)) Add(Role::OperatorBoundaryParen);
	if(Language == Lang::Go && Name == "composite_literal") Add(Role::NamedCompositeBody);
	if(Language == Lang::Rust && Name == "struct_expression" || Language == Lang::Swift && Name == "lambda_literal") {
		Add(Role::HeaderBodyLikeValue);
	}
	if(Language == Lang::Swift && Name == "tuple_expression") Add(Role::TransparentTupleParen);
	if((Language.IsJsTs() || Language == Lang::Swift) && Has(Role::TypeCastRight)) Add(Role::ReopenableTypeCast);
	if(Language == Lang::Swift && Has(Role::GroupingTypeParen)) Add(Role::WrappedTypeItem);
	if(Language.IsJsTs() && Has(Role::GroupingTypeParen)) Add(Role::AlgebraicTypeParen);
	if(
		(Language.IsJsTs() || Language == Lang::Go || Language == Lang::Rust || Language == Lang::Kotlin || Language == Lang::Swift) &&
		Has(Role::GroupingTypeParen)
	) Add(Role::RemovableTypeParen);
	if(Language == Lang::Swift && Name == "function_type") Add(Role::TupleParameterFunctionType);
	if(Language == Lang::Go && Has(Role::GroupingTypeParen)) Add(Role::ConversionTypeParen);
	if(Language == Lang::Swift && (Name == "pattern" || Has(Role::TightHeaderEquals))) Add(Role::RemovablePatternParen);
	if((Language == Lang::Go || Language == Lang::Rust || Language == Lang::Swift) && Has(Role::Control)) {
		Add(Role::BodyAmbiguousControl);
	}
	if(Language == Lang::Java && Name == "local_variable_declaration") Add(Role::FinalizableLocal);
	if(Language == Lang::Kotlin && Name == "property_declaration") Add(Role::ReplaceableBindingKeyword);
	if(Language == Lang::Kotlin && Name == "do_while_statement") Add(Role::NestedLoopBodyBraces);
	if(Name == "catch_type") Add(Role::CatchTypeAlternatives);
	if(Name == "disjunctive_normal_form_type") Add(Role::NormalFormType);
	if(Name == "not_nullable_type") Add(Role::NonNullableType);
	if(Name == "type_bound") Add(Role::TypeBounds);
	if(Name == "type_elem") Add(Role::TypeElements);
	if(Name == "type_list") Add(Role::TypeList);
	if(Name == "pattern") Add(Role::Pattern);
	if(Language == Lang::Rust && Name == "identifier") Add(Role::NormalizedBindingName);
	if(Language == Lang::Rust && Name == "reference_expression") Add(Role::BorrowExpression);
	if(Language == Lang::Rust && Name == "call_expression") Add(Role::ReceiverMutationCall);
	if(Language == Lang::Rust && Name == "match_expression") Add(Role::BorrowingMatch);
	if(Language == Lang::Rust && Names::RustLetBinding.Contains(Name)) Add(Role::MutablePatternBinding);
	// 入力側の読取（整形仕様 4.3，入力側と検証側は節点名を照合せず此の役割を読む）
	if(Language == Lang::Ruby) {
		if(Names::RubyParameters.Contains(Name)) Add(Role::LocalParameters);
		if(Names::RubyBinding.Contains(Name)) Add(Role::BindingExpression);
		if(Names::RubyLocalScope.Contains(Name)) Add(Role::LocalScope);
		if(Names::RubyFreshScope.Contains(Name)) Add(Role::IsolatedScope);
		if(Names::RubyCommandArgumentHost.Contains(Name)) Add(Role::CommandArgumentHost);
		if(Names::RubyArgumentExtent.Contains(Name)) Add(Role::CommandArgumentExtent);
		if(!IsNamed && Names::RubyLowBinary.Contains(Name)) Add(Role::LowPrecedenceBinaryKeyword);
		if(Names::RubyArgumentSequence.Contains(Name)) Add(Role::ElementSequence);
		if(IsNamed && Names::RubyArgumentListHost.Contains(Name)) Add(Role::ArgumentListHost);
		if(Names::RubyArgumentKeyword.Contains(Name)) Add(Role::ValueTakingJump);
		if(!IsNamed && Names::RubyArgumentStart.Contains(Name)) Add(Role::ArgumentStartToken);
	}
	if(!IsNamed && Names::ContinuingKeyword.Contains(Name)) Add(Role::ContinuingWord);
	if(Language.IsJsTs() && !IsNamed && Names::JsRestrictedKeyword.Contains(Name)) Add(Role::RestrictedLineWord);
	if(Names::MacroBody.Contains(Name)) Add(Role::MacroBody);
	if(Names::MacroDefinition.Contains(Name)) Add(Role::MacroDefinition);
	if(Names::PreprocNameQuery.Contains(Name)) Add(Role::MacroNameQuery);
	if(Language.IsCFamily() && Names::CNameDeclaration.Contains(Name)) Add(Role::NameDeclaration);
	if(Language.IsCFamily() && Name == "declaration") Add(Role::PotentialExpressionDeclaration);
	// C の構造体・共用体・列挙の名前は `struct` 等を伴わないと型に為らない為，指定子が裸の型名を導入するのは C++ だけ
	if(
		Language.IsCFamily() && Names::CTypeAliasDeclaration.Contains(Name) ||
		Language == Lang::Cpp && Names::CppTypeSpecifier.Contains(Name)
	) Add(Role::BareTypeDeclaration);
	if(Names::Leaf.Contains(Name)) Add(Role::SourceLeaf);
	if(Names::Preproc.Contains(Name)) Add(Role::PreprocessorNode);
	if(Language == Lang::Go && Names::GoDocumentedDeclaration.Contains(Name)) Add(Role::DocumentedTopDeclaration);
	if(Names::MultilineCommaContainer.Contains(Name)) Add(Role::DelimitedList);
	if(Language == Lang::CSS && Names::ScssParameterHost.Contains(Name)) Add(Role::ParameterRule);
	if(Language == Lang::CSS && Names::ScssModuleRule.Contains(Name)) Add(Role::ModuleRule);
	if(Language.IsCFamily() && Names::CDeclaratorWrapper.Contains(Name)) Add(Role::NestingDeclarator);
	if(Language == Lang::Cpp && Names::CppScopeBoundary.Contains(Name)) Add(Role::TemplateScanBoundary);
	if(Language == Lang::Kotlin && (Name == "constructor_invocation" || Name == "spread_expression")) {
		Add(Role::AdjacencyRecordingHost);
	}
	if(Language == Lang::Ruby && Name == "unary") Add(Role::AdjacencyRecordingHost);
	if(Language == Lang::Ruby && Name == "argument_list") Add(Role::AdjacencyRecordingOperand);
	if(Names::ReturnStatement.Contains(Name)) Add(Role::ReturningStatement);
	if(Language == Lang::Go && Names::GoFunctionDecl.Contains(Name)) Add(Role::PragmaTargetFunction);
	if(Names::ClauseBody.Contains(Name)) Add(Role::BodyEndingClause);
	if(Names::BraceClause.Contains(Name)) Add(Role::FollowingBodyClause);
	if(Language == Lang::PHP && Names::PhpIfBranch.Contains(Name)) Add(Role::IfFollowingBranch);
	if(Names::CommentLiteralHost.Contains(Name)) Add(Role::CommentKeepingHost);
	if(Language == Lang::Ruby && Names::RubyDelimitedLiteral.Contains(Name)) Add(Role::DelimitedLiteral);
	if(Names::SequentialStmtContainer.Contains(Name)) Add(Role::SequentialStatements);
	if(Names::TransparentWrap.Contains(Name)) Add(Role::TransparentWrapper);
	if(Names::JsStatementBlockOrBlock.Contains(Name)) Add(Role::BracedStatementBlock);
	if(Names::ThenDo.Contains(Name)) Add(Role::BodyTriggerWord);
	if(Language.IsJsTs() && Names::JsMemberHost.Contains(Name)) Add(Role::OperandHeadedAccess);
	if(Language == Lang::HTML && Names::HtmlRawTextElement.Contains(Name)) Add(Role::EmbeddedCodeElement);
	if(Language == Lang::Java && Names::JavaEscapeHost.Contains(Name)) Add(Role::UnicodeEscapeHost);
	if(Language == Lang::Java && Names::JavaExhaustiveCaseLabel.Contains(Name)) Add(Role::ExhaustiveCaseLabel);
	if(Language == Lang::HTML && Names::HtmlElementWrap.Contains(Name)) Add(Role::MarkupElementNode);
	if(Language.IsJsTs() && Names::JsStatementSequence.Contains(Name)) Add(Role::StatementOrMemberSequence);
	if(Language == Lang::Swift) {
		if(Names::SwiftLiteralText.Contains(Name)) Add(Role::OpaqueLiteral);
		if(Has(Role::TightHeaderEquals)) Add(Role::BracedHeaderStatement);
		if(Names::SwiftStrayBlock.Contains(Name)) Add(Role::StrayBlock);
		if(!IsNamed && Names::SwiftHeaderLead.Contains(Name)) Add(Role::HeaderExpressionLead);
	}
	if(Language == Lang::CSharp && Names::CsNameType.Contains(Name)) Add(Role::ExpressionTypeName);
	if(Language.IsJsTs() && Names::JsModuleStatement.Contains(Name)) Add(Role::ModuleStatement);
	if(Language.IsJsTs() && Name == "html_comment") Add(Role::UnsupportedComment);
	if(Name == "line_continuation") Add(Role::LineContinuation);
	if(Name == "array_rank_specifier") Add(Role::ArrayRank);
	if(Name == "array_type") Add(Role::ArrayTypeSyntax);
	if(Name == "block_parameters") Add(Role::BlockParameterList);
	if(Name == "callable_reference") Add(Role::CallableReference);
	if(Name == "comment") Add(Role::CommentNode);
	if(Name == "composite_literal") Add(Role::CompositeLiteral);
	if(Name == "const_declaration") Add(Role::ConstantDeclarationNode);
	if(Name == "const_spec") Add(Role::ConstantSpecification);
	if(Name == "declaration_expression") Add(Role::DeclarationExpression);
	if(Name == "doctype") Add(Role::DocumentTypeDeclaration);
	if(Name == "enumerator") Add(Role::EnumerationItem);
	if(Name == "erroneous_end_tag") Add(Role::ErroneousEndTag);
	if(Name == "func_literal") Add(Role::FunctionLiteral);
	if(Name == "function_declaration") Add(Role::FunctionDeclarationNode);
	if(Name == "heredoc_body") Add(Role::HeredocBody);
	if(Name == "if_statement") Add(Role::IfStatementNode);
	if(Name == "import_declaration") Add(Role::ImportDeclarationNode);
	if(Name == "include_statement") Add(Role::IncludeStatementNode);
	if(Name == "integer_value") Add(Role::IntegerLiteralValue);
	if(Name == "interpolation_format_clause") Add(Role::InterpolationFormat);
	if(Name == "label_name") Add(Role::LabelName);
	if(Name == "left_assignment_list") Add(Role::LeftAssignmentList);
	if(Name == "line_comment") Add(Role::LineCommentNode);
	if(Name == "metatype") Add(Role::MetatypeNode);
	if(Name == "object_literal") Add(Role::ObjectLiteralNode);
	if(Name == "placeholder") Add(Role::PlaceholderNode);
	if(Name == "prefix_unary_expression") Add(Role::PrefixUnaryOperation);
	if(Name == "preproc_call") Add(Role::PreprocessorCall);
	if(Name == "preproc_function_def") Add(Role::PreprocessorFunctionDefinition);
	if(Name == "preproc_include") Add(Role::PreprocessorInclude);
	if(Name == "property_name") Add(Role::PropertyNameNode);
	if(Name == "qualified_name") Add(Role::QualifiedNameNode);
	if(Name == "raw_text") Add(Role::RawTextNode);
	if(Name == "receive_statement") Add(Role::ReceiveStatement);
	if(Name == "regex_literal") Add(Role::RegexLiteralNode);
	if(Name == "rule_set") Add(Role::SelectorRule);
	if(Name == "scoped_identifier") Add(Role::ScopedIdentifier);
	if(Name == "selectors") Add(Role::SelectorList);
	if(Name == "self_closing_tag") Add(Role::SelfClosingTag);
	if(Name == "short_var_declaration") Add(Role::ShortVariableDeclaration);
	if(Name == "source_file") Add(Role::SourceFileNode);
	if(Name == "string_literal") Add(Role::StringLiteralNode);
	if(Name == "struct_expression") Add(Role::StructExpression);
	if(Name == "super") Add(Role::SuperReference);
	if(Name == "var_declaration") Add(Role::VariableDeclarationNode);
	if(Name == "var_spec") Add(Role::VariableSpecification);
	if(Name == "var_spec_list") Add(Role::VariableSpecificationList);
	if(Name == "variadic_parameter_declaration") Add(Role::VariadicParameterDeclaration);
	if(Name == "when_condition") Add(Role::WhenCondition);
	if(Name == "where_clause") Add(Role::WhereClauseNode);
	if(Name == "(") Add(Role::OpeningParenToken);
	if(Name == "}") Add(Role::ClosingBraceToken);
	if(Name == ">") Add(Role::GreaterThanToken);
	if(Name == "<") Add(Role::LessThanToken);
	if(Name == "::") Add(Role::DoubleColonToken);
	if(Name == "=") Add(Role::AssignmentToken);
	if(Name == ":=") Add(Role::ShortAssignmentToken);
	if(Name == ".") Add(Role::DotToken);
	if(Name == ",") Add(Role::CommaToken);
	if(Name == "arrow_function") Add(Role::ArrowFunctionNode);
	if(Name == "tag_name") Add(Role::TagNameNode);
	if(Name == "php_end_tag") Add(Role::PhpEndTag);
	if(Name == "start_tag") Add(Role::StartTag);
	if(Name == "non_null_expression") Add(Role::NonNullExpression);
	if(Name == "subscript_expression") Add(Role::SubscriptExpressionNode);
	if(Name == "macro_rules!") Add(Role::MacroRulesToken);
	if(Name == "[") Add(Role::OpeningBracketToken);
	if(Name.starts_with('>')) Add(Role::AngleCloseToken);
	if(Language == Lang::Swift && Name == "infix_expression") Add(Role::SpacingSensitiveInfix);
	if(Language == Lang::CSS && Name == "ERROR") Add(Role::LaggingGrammarError);
	if(Language == Lang::Python && Name == "type_alias_statement") Add(Role::TypeAliasStatement);
	if(Language == Lang::Python && Name == "string") Add(Role::VerbatimQuotedString);
	if(Language == Lang::Rust && Name == "macro_definition") Add(Role::MacroRuleDefinition);
	if(Language.IsCFamily() && Name == "template_type") Add(Role::TemplateInstantiationType);
	if(Language == Lang::Cpp && Name == "template_declaration") Add(Role::TypeParameterScope);
	if(Language == Lang::Cpp && Name == "namespace_identifier") Add(Role::NamespaceName);
	if(Language == Lang::Go && Name == "short_var_declaration") Add(Role::ReassigningShortDeclaration);
	if(Language == Lang::Swift && (Name == "metatype" || Name == "user_type")) Add(Role::SpacelessTypeName);
	if(Language == Lang::CSharp && Name == "pointer_type") Add(Role::ProductAmbiguousPointerType);
	if(Language.IsCFamily() && Name == "parenthesized_expression") Add(Role::MacroGuardedParentheses);
	if(Language == Lang::CSS && Name == "identifier") Add(Role::PlainValueWord);
	if(Language == Lang::Ruby && Name == "uninterpreted") Add(Role::TrailingDataSection);
	if(Language == Lang::Swift && Name == "range_expression") Add(Role::SpacingSensitiveRange);
	if(Language.IsJsTs() && IsNamed && Names::JsLetBindingHead.Contains(Name)) Add(Role::SplitBindingHead);
	if(Names::VerbatimNode.Contains(Name)) Add(Role::SourceCopiedNode);
	if(Names::Unformattable.Contains(Name)) Add(Role::VerbatimSubtree);
	if(Language == Lang::Kotlin) {
		if(Names::KotlinImportLeaf.Contains(Name)) Add(Role::ImportLeaf);
		if(Names::KotlinStatementContext.Contains(Name)) Add(Role::LineTerminatedContext);
		if(Names::KotlinJointSuffix.Contains(Name)) Add(Role::LineJoiningSuffix);
		if(Names::KotlinClassMember.Contains(Name)) Add(Role::ClassMember);
	}
	if(Language == Lang::HTML && Name == "script_element") Add(Role::EmbeddedScriptElement);
	// 字句の折返位置と区切りを節点の役割として記録する
	static constexpr std::array<std::string_view, 5> LeadingBreakTokens { ")", ".", "?.", "]", "}" };
	static constexpr std::array<std::string_view, 6> TrailingBreakTokens { "(", ",", ";", "=>", "[", "{" };
	static constexpr std::array<std::string_view, 42> TrailingOperatorTokens {
		"!=",
		"!==",
		"%",
		"%=",
		"&",
		"&&",
		"&&=",
		"&=",
		"*",
		"**",
		"**=",
		"*=",
		"+",
		"+=",
		"-",
		"-=",
		"/",
		"/=",
		":",
		"<",
		"<<",
		"<<=",
		"<=",
		"<=>",
		"=",
		"==",
		"===",
		">",
		">=",
		">>",
		">>=",
		">>>",
		">>>=",
		"?",
		"?\?",
		"?\?=",
		"^",
		"^=",
		"|",
		"|=",
		"||",
		"||="
	};
	if(std::ranges::find(LeadingBreakTokens, Name) != LeadingBreakTokens.end()) Add(Role::LeadingBreakToken);
	if(std::ranges::find(TrailingBreakTokens, Name) != TrailingBreakTokens.end()) Add(Role::TrailingBreakToken);
	if(std::ranges::find(TrailingOperatorTokens, Name) != TrailingOperatorTokens.end()) Add(Role::TrailingOperatorToken);
	if(Name == "->" || Name == "?->") Add(Role::MemberAccessArrow);
	if(Name == ";") Add(Role::StatementTerminatorToken);
	if(Name == "{") Add(Role::OpeningBraceToken);
	if(Name == ")") Add(Role::ClosingParenToken);
	if(Name == "]") Add(Role::ClosingSquareToken);
	if(Name == ":") Add(Role::ColonToken);
	if(Name == "?") Add(Role::QuestionToken);
	if(Name == "!") Add(Role::ExclamationToken);
	if(Name == "/>") Add(Role::SelfClosingTagToken);
	if(Name == "**") Add(Role::ExponentToken);
	if(Name == "...") Add(Role::SpreadToken);
	if(Name == ".." || Name == "...") Add(Role::OpenRangeToken);
	if(Name == "*") Add(Role::AsteriskToken);
	if(Name == "&") Add(Role::AmpersandToken);
	if(Name == "|") Add(Role::PipeToken);
	if(Name == "??") Add(Role::CoalescingToken);
	if(Name == "?.") Add(Role::OptionalAccessToken);
	if(Name == "&.") Add(Role::SafeAccessToken);
	if(Name == "->") Add(Role::RightArrowToken);
	if(Name == "=>") Add(Role::FatArrowToken);
	if(Name == "?->") Add(Role::OptionalArrowToken);
	if(Name == "/") Add(Role::SlashToken);
	if(Name == "</") Add(Role::ClosingTagOpenToken);
	if(Name == "@") Add(Role::AtToken);
	if(Name == "\\") Add(Role::BackslashToken);
	if(Name == "#") Add(Role::HashToken);
	if(Name == "#[") Add(Role::AttributeOpenToken);
	if(Name == "<!") Add(Role::MarkupDeclarationOpenToken);
	if(Name == "${") Add(Role::InterpolationOpenToken);
	if(Name == "[[") Add(Role::DoubleSquareOpenToken);
	if(Name == "]]") Add(Role::DoubleSquareCloseToken);
	if(Name == "~") Add(Role::TildeToken);
	if(!IsNamed && Name == "_") Add(Role::WildcardToken);
	if(Name == "++" || Name == "--") Add(Role::ChangeByOneToken);
	if(Name == "then") Add(Role::ConditionBodyDelimiterToken);
	if(Name == "do") Add(Role::IterationBodyDelimiterToken);
	// 字句の端の記号の形（配置の段が挿入位置の前後の繋がりを判ずる，整形仕様 7.6）
	if(!IsNamed && !Name.empty()) {
		const char First = Name.front(), Last = Name.back();
		if(First == '(' || First == '[' || First == '{') Add(Role::OpeningBracketLead);
		if(First == '(') Add(Role::OpeningParenLead);
		if(First == '[') Add(Role::OpeningSquareLead);
		if(First == '{') Add(Role::OpeningBraceLead);
		if(First == ')' || First == ']' || First == '}') Add(Role::ClosingBracketLead);
		if(First == ']') Add(Role::ClosingSquareLead);
		if(Names::UnaryPrefixToken.Contains(Name.substr(0, 1))) Add(Role::UnaryPrefixLead);
		if(First == '<') Add(Role::OpeningAngleLead);
		if(First == ';') Add(Role::StatementTerminatorLead);
		if(First == ',') Add(Role::CommaLead);
		if(First == ':') Add(Role::ColonLead);
		if(First == '?') Add(Role::QuestionLead);
		if(First == '|') Add(Role::PipeLead);
		if(First == '}') Add(Role::ClosingBraceLead);
		if(First == ')') Add(Role::ClosingParenLead);
		if(std::string_view("+-*/%<>=!&|^~?.").find(First) != std::string_view::npos) Add(Role::ExpressionContinuationLead);
		if(First == ',' || First == ';') Add(Role::SeparatorLead);
		if(Name.starts_with("()")) Add(Role::EmptyParenthesesLead);
		if(Last == '(' || Last == '[' || Last == '{') Add(Role::OpeningBracketTail);
		if(Last == ')') Add(Role::ClosingParenTail);
		if(Last == ';') Add(Role::StatementTerminatorTail);
		if(Last == ',') Add(Role::CommaTail);
		if(Last == '{') Add(Role::OpeningBraceTail);
		if(
			std::string_view("([{,;:=?+-*%&|^~<>.").find(Last) != std::string_view::npos &&
			!(Name.size() > 1 && (Last == '+' || Last == '-') && Name[Name.size() - 2] == Last)
		) Add(Role::ExpressionContinuationTail);
	}
	if(!IsNamed) {
		if(Names::StringDelimiter.Contains(Name)) Add(Role::QuoteToken);
		else if(Name.size() > 1 && Name.back() == '"') if(
			const std::span<const std::string_view> Prefixes = StringPrefixes(Language);
			std::ranges::find(Prefixes, Name.substr(0, Name.size() - 1)) != Prefixes.end()
		) Add(Role::QuoteToken);
	}
	if(Language == Lang::Swift) {
		static constexpr std::array<std::string_view, 10> BinaryOperations {
			"additive_expression",
			"bitwise_operation",
			"comparison_expression",
			"conjunction_expression",
			"disjunction_expression",
			"equality_expression",
			"infix_expression",
			"multiplicative_expression",
			"nil_coalescing_expression",
			"range_expression"
		};
		static constexpr std::array<std::string_view, 3> PostfixOperands {
			"additive_expression",
			"multiplicative_expression",
			"prefix_expression"
		};
		static constexpr std::array<std::string_view, 3> PostfixExpressions {
			"call_expression",
			"navigation_expression",
			"postfix_expression"
		};
		static constexpr std::array<std::string_view, 8> PrecedenceBoundaries {
			"as_expression",
			"assignment",
			"await_expression",
			"check_expression",
			"consume_expression",
			"open_end_range_expression",
			"open_start_range_expression",
			"try_expression"
		};
		if(std::ranges::find(BinaryOperations, Name) != BinaryOperations.end()) Add(Role::BinaryPrecedenceExpression);
		if(std::ranges::find(PostfixOperands, Name) != PostfixOperands.end()) Add(Role::PostfixBindingOperand);
		if(std::ranges::find(PostfixExpressions, Name) != PostfixExpressions.end()) Add(Role::PostfixBindingExpression);
		if(std::ranges::find(PrecedenceBoundaries, Name) != PrecedenceBoundaries.end()) Add(Role::OperatorPrecedenceBoundary);
		if(Name == "ternary_expression") Add(Role::ConditionalOperatorExpression);
	}
	if(Language == Lang::HTML && Name == "element") Add(Role::ContentElement);
	if(Language == Lang::Java && (Name == "character_literal" || Name == "string_literal")) Add(Role::EscapeLiteralHost);
	if(Language == Lang::Java && Name == "block_comment") Add(Role::BlockCommentNode);
	if(Names::StructuralParenBoundary.Contains(Name)) Add(Role::StructuralParenBoundary);
	if(Names::MeaningfulParenGroup.Contains(Name)) Add(Role::MeaningfulParenGroup);
	if(Names::ReceiverParenHost.Contains(Name)) Add(Role::ReceiverParenHost);
	if(Name == "<-") Add(Role::ChannelReceiveToken);
	if(Name == "!!") Add(Role::NonNullAssertionToken);
	if(!IsNamed && !Name.empty()) {
		const char First = Name.front();
		if(Name.back() == '*') Add(Role::PointerMemberAccess);
		if(std::string_view("<>+-.[(`").find(First) != std::string_view::npos) Add(Role::InstantiationContinuationLead);
		if(Name.find_first_of("<>") != std::string_view::npos) Add(Role::AngleConflictToken);
		if(Name.find('>') != std::string_view::npos) Add(Role::AngleCloseCandidate);
		if(std::string_view("-+*&^").find(First) != std::string_view::npos) Add(Role::CastOperandLead);
		if(
			std::string_view("*&-+").find(First) != std::string_view::npos && !(Name.size() > 1 && Name[1] == '=') &&
			!Name.starts_with("->")
		) Add(Role::CastContinuationToken);
		if(First == '*' || First == '(') Add(Role::CastUpdateFollowerLead);
		if(Name.find(':') != std::string_view::npos) Add(Role::InterpolationFormatSeparator);
		if(std::string_view("\"'@$").find(First) != std::string_view::npos) Add(Role::InterpolationOpaqueLead);
		if(Name == ":]") Add(Role::ClosedValueTerminator);
		if(std::string_view("(`+-[").find(First) != std::string_view::npos) Add(Role::TypeArgumentOpeningLead);
		if(std::string_view("\"#'`([{").find(First) != std::string_view::npos) Add(Role::BodyDelimiterLead);
		if(First == '.') Add(Role::MemberAccessLead);
		if(Name.back() == '.') Add(Role::MemberAccessTail);
	}
	if(!IsNamed) {
		if(Name == "{") Add(Role::OpenBrace);
		if(Name.size() == 1 && std::string_view(")]},;").find(Name) != std::string_view::npos) Add(Role::ExpressionTerminator);
		else if(Name == "else") Add(Role::ElseKeyword);
		else if(Name == "using") Add(Role::UsingKeyword);
		// 字句の役割の返戻
		return Roles;
	}
	// 文と要素と型に固有の役割
	if(Language.IsJsTs()) {
		static constexpr std::array<std::string_view, 13> Statements {
			"break_statement",
			"continue_statement",
			"debugger_statement",
			"do_statement",
			"export_statement",
			"expression_statement",
			"function_signature",
			"import_statement",
			"lexical_declaration",
			"return_statement",
			"throw_statement",
			"type_alias_declaration",
			"variable_declaration"
		};
		static constexpr std::array<std::string_view, 5> Members {
			"abstract_method_signature",
			"field_definition",
			"index_signature",
			"method_signature",
			"public_field_definition"
		};
		if(std::ranges::binary_search(Statements, Name)) Add(Role::TerminatedStatement);
		if(std::ranges::binary_search(Members, Name)) Add(Role::TerminatedMember);
		if(Name == "jsx_text") Add(Role::CollapsibleText);
		if(Name == "jsx_element") Add(Role::MarkupElement);
		if(Name == "jsx_opening_element") Add(Role::MarkupOpen);
		if(Name == "jsx_closing_element") Add(Role::MarkupClose);
		if(Name == "jsx_expression") Add(Role::MarkupExpression);
		if(Name == "jsx_attribute") Add(Role::MarkupAttribute);
		if(Name == "non_null_expression") Add(Role::NonNullAssertion);
		if(Name == "required_parameter") Add(Role::RequiredParameter);
		if(Name == "type_parameters") Add(Role::TypeParameterList);
		if(Name == "type_arguments") Add(Role::TypeArgumentList);
		if(Name == "class_body") Add(Role::MemberBody);
		if(Name == "binary_expression") Add(Role::TypeofComparison);
		if(Name == "named_imports") Add(Role::NamedImports);
		if(Name == "arrow_function") Add(Role::ArrowFunction);
		if(Name == "new_expression") Add(Role::Construction);
		if(Name == "export_statement") Add(Role::Export);
		if(Name == "internal_module") Add(Role::Namespace);
		if(Name == "for_in_statement") Add(Role::Iteration);
		if(Name == "as_expression" || Name == "type_assertion" || Name == "satisfies_expression") Add(Role::TypeAssertion);
		if(Name == "instantiation_expression") Add(Role::Instantiation);
	}
	if(Language == Lang::TypeScript) {
		static constexpr std::array<std::string_view, 5> TypeMembers {
			"call_signature",
			"construct_signature",
			"index_signature",
			"method_signature",
			"property_signature"
		};
		if(Name == "interface_body" || Name == "object_type") Add(Role::TypeMemberBody);
		if(std::ranges::binary_search(TypeMembers, Name)) Add(Role::TypeMember);
	}
	// 複数文法で共有する構文の集合
	const bool IsKotlin = Language == Lang::Kotlin;
	static constexpr std::array<std::string_view, 10> CommaDeclarations {
		"const_declaration",
		"declaration",
		"field_declaration",
		"lexical_declaration",
		"local_declaration_statement",
		"local_variable_declaration",
		"property_declaration",
		"short_var_declaration",
		"var_declaration",
		"variable_declaration"
	};
	static constexpr std::array<std::string_view, 4> BracedBlocks {
		"block",
		"compound_statement",
		"control_structure_body",
		"statement_block"
	};
	static constexpr std::array<std::string_view, 16> BodyOwners {
		"do_statement",
		"do_while_statement",
		"else_clause",
		"else_if_clause",
		"enhanced_for_statement",
		"fixed_statement",
		"for_in_statement",
		"for_range_loop",
		"for_statement",
		"foreach_statement",
		"if_expression",
		"if_statement",
		"labeled_statement",
		"lock_statement",
		"using_statement",
		"while_statement"
	};
	static constexpr std::array<std::string_view, 28> BodyDeclarations {
		"abstract_class_declaration",
		"alias_declaration",
		"class_declaration",
		"class_specifier",
		"declaration",
		"enum_declaration",
		"enum_specifier",
		"function_declaration",
		"function_definition",
		"function_static_declaration",
		"generator_function_declaration",
		"global_declaration",
		"interface_declaration",
		"lexical_declaration",
		"local_declaration_statement",
		"local_function_statement",
		"local_variable_declaration",
		"namespace_alias_definition",
		"property_declaration",
		"record_declaration",
		"static_assert_declaration",
		"struct_specifier",
		"trait_declaration",
		"type_alias_declaration",
		"type_definition",
		"union_specifier",
		"using_declaration",
		"variable_declaration"
	};
	static constexpr std::array<std::string_view, 11> DefaultParameters {
		"assignment_pattern",
		"class_parameter",
		"default_parameter",
		"function_value_parameters",
		"optional_parameter",
		"optional_parameter_declaration",
		"parameter",
		"property_promotion_parameter",
		"required_parameter",
		"simple_parameter",
		"typed_default_parameter"
	};
	static constexpr std::array<std::string_view, 7> TightEquals {
		"attribute",
		"attribute_argument",
		"declare_directive",
		"keyword_argument",
		"keyword_pattern",
		"setter",
		"value_argument"
	};
	static constexpr std::array<std::string_view, 12> Loops {
		"do_statement",
		"do_while_statement",
		"enhanced_for_statement",
		"for_expression",
		"for_in_statement",
		"for_range_loop",
		"for_statement",
		"foreach_statement",
		"loop_expression",
		"repeat_while_statement",
		"while_expression",
		"while_statement"
	};
	static constexpr std::array<std::string_view, 19> Preprocessors {
		"preproc_call",
		"preproc_def",
		"preproc_define",
		"preproc_elif",
		"preproc_elifdef",
		"preproc_else",
		"preproc_endregion",
		"preproc_error",
		"preproc_function_def",
		"preproc_if",
		"preproc_if_in_attribute_list",
		"preproc_ifdef",
		"preproc_include",
		"preproc_line",
		"preproc_nullable",
		"preproc_pragma",
		"preproc_region",
		"preproc_undef",
		"preproc_warning"
	};
	static constexpr std::array<std::string_view, 5> PreprocessorBranches {
		"preproc_elif",
		"preproc_elifdef",
		"preproc_else",
		"preproc_if",
		"preproc_ifdef"
	};
	static constexpr std::array<std::string_view, 13> PreprocessorLines {
		"preproc_call",
		"preproc_def",
		"preproc_define",
		"preproc_endregion",
		"preproc_error",
		"preproc_function_def",
		"preproc_include",
		"preproc_line",
		"preproc_nullable",
		"preproc_pragma",
		"preproc_region",
		"preproc_undef",
		"preproc_warning"
	};
	static constexpr std::array<std::string_view, 22> SectionHeaders {
		"access_specifier",
		"case_statement",
		"communication_case",
		"default_case",
		"default_statement",
		"else",
		"elsif",
		"ensure",
		"expression_case",
		"in_clause",
		"labeled_statement",
		"named_label_statement",
		"rescue",
		"statement_label",
		"switch_block_statement_group",
		"switch_case",
		"switch_default",
		"switch_entry",
		"switch_label",
		"switch_section",
		"type_case",
		"when"
	};
	static constexpr std::array<std::string_view, 11> StatementHosts {
		"block",
		"block_body",
		"body_statement",
		"compound_statement",
		"control_structure_body",
		"program",
		"source_file",
		"statement_block",
		"statement_list",
		"statements",
		"translation_unit"
	};
	static constexpr std::array<std::string_view, 5> TrailingCommaHosts {
		"argument_list",
		"literal_value",
		"parameter_list",
		"type_arguments",
		"type_parameter_list"
	};
	if(Language == Lang::Python) {
		if(Name == "type") Add(Role::TypeSyntax);
		if(Name == "generic_type") Add(Role::TypeApplication);
		if(Name == "type_alias_statement") Add(Role::TypeAlias);
		if(Name == "call" || Name == "subscript") Add(Role::PostfixReceiverHost);
		if(Name == "assignment" || Name == "augmented_assignment") Add(Role::Assignment);
		if(Name == "expression_list" || Name == "pattern_list" || Name == "tuple_expression") Add(Role::BareTuple);
		if(
			Name == "binary_operator" || Name == "boolean_operator" || Name == "comparison_operator" || Name == "conditional_expression" ||
			Name == "concatenated_string" || Name == "lambda" || Name == "expression_list" || Name == "pattern_list" ||
			Name == "attribute" || Name == "type" || Name == "tuple_expression"
		) Add(Role::ContinuationExpression);
	}
	if(Language == Lang::Python && Name == "block") Add(Role::IndentedBody);
	if(
		Language == Lang::Python &&
		(Name == "elif_clause" || Name == "except_clause" || Name == "finally_clause" || Name == "else_clause")
	) Add(Role::Clause);
	if(Language == Lang::PHP) {
		if(Name == "php_tag") Add(Role::CodeOpener);
		if(Name == "text_interpolation") Add(Role::CodeBoundary);
		if(Name == "static_variable_declaration") Add(Role::DeclaratorItem);
		if(Name == "function_static_declaration") {
			Add(Role::CommaDeclaration);
			Add(Role::Declaration);
		}
		if(Names::PhpSemicolonStatement.Contains(Name)) Add(Role::TerminatedStatement);
	}
	if(Language == Lang::PHP && Name == "colon_block") Add(Role::DelimitedBody);
	if(Name == "ERROR") Add(Role::Error);
	if(Name == "annotation" || Name == "file_annotation" || Name == "use_site_target") Add(Role::AnnotationParts);
	if(Name == "assignment_expression") Add(Role::Assignment);
	if(Name == "await_expression") Add(Role::Await);
	if(Language == Lang::Cpp && (Name == "placeholder_type_specifier" || Name == "type_identifier")) Add(Role::DeducedType);
	if(std::find(DefaultParameters.begin(), DefaultParameters.end(), Name) != DefaultParameters.end()) {
		Add(Role::DefaultParameter);
		Add(Role::EqualsSpaced);
	}
	if(Name == "public_field_definition") Add(Role::EqualsSpaced);
	if(std::find(TightEquals.begin(), TightEquals.end(), Name) != TightEquals.end()) Add(Role::EqualsTight);
	if(std::ranges::binary_search(BracedBlocks, Name)) Add(Role::Block);
	if(std::ranges::binary_search(BodyOwners, Name)) Add(Role::BodyOwner);
	if(Name == "control_structure_body") Add(Role::BodyWrapper);
	// 枝の値を結果にする本体・枝・ラムダ・try 式と値を持たない文は，其れらを式とする文法の物だけ
	if(IsKotlin && (Name == "catch_block" || Name == "control_structure_body")) Add(Role::BranchBody);
	if(IsKotlin && Name == "when_entry") Add(Role::CaseArm);
	if(IsKotlin && Name == "lambda_literal") Add(Role::LastValueLambda);
	if(IsKotlin && (Name == "assignment" || Name == "for_statement" || Name == "while_statement")) Add(Role::StatementOnly);
	if(IsKotlin && Name == "try_expression") Add(Role::ValueTry);
	if(std::ranges::binary_search(BodyDeclarations, Name)) Add(Role::Declaration);
	if(std::ranges::binary_search(CommaDeclarations, Name)) {
		Add(Role::CommaDeclaration);
		Add(Role::Declaration);
	}
	if(Name == "do_statement" || Name == "do_while_statement") Add(Role::DoLoop);
	if(Name == "else_clause") Add(Role::ElseClause);
	if(Name == "else_if_clause") Add(Role::ElseIfClause);
	if(Name == "expression_statement") Add(Role::ExpressionStatement);
	if(Name == "if_expression" || Name == "if_statement") Add(Role::If);
	// 地の文は PHP の文法の物だけ（HTML の本文と区別する）
	if(Language == Lang::PHP && (Name == "text" || Name == "text_interpolation")) Add(Role::InlineText);
	if(Name == "annotation" || Name == "label" || Name == "statement_label") Add(Role::Label);
	if(Name == "labeled_statement" || Name == "named_label_statement") Add(Role::Labeled);
	if(std::ranges::binary_search(Loops, Name)) Add(Role::Loop);
	if(Language == Lang::CSS && (Name == "property_name" || Name == "tag_name")) Add(Role::NameParts);
	if(Name == "prefix_expression") Add(Role::PrefixOperation);
	if(std::ranges::binary_search(Preprocessors, Name)) Add(Role::Preprocessor);
	if(std::ranges::binary_search(PreprocessorBranches, Name)) Add(Role::PreprocessorBranch);
	if(std::ranges::binary_search(PreprocessorLines, Name)) Add(Role::PreprocessorLine);
	if(Language == Lang::Rust && (Name == "token_tree_pattern" || Name == "token_tree")) Add(Role::PreservedTokenSequence);
	if(std::ranges::binary_search(SectionHeaders, Name)) {
		Add(Role::SectionHeader);
		Add(Role::ClauseHeading);
	}
	if(std::ranges::binary_search(StatementHosts, Name)) Add(Role::StatementHost);
	if(Name == "statements") Add(Role::StatementSequence);
	if(Language == Lang::Go && std::ranges::binary_search(TrailingCommaHosts, Name)) Add(Role::TrailingCommaHost);
	if(Name == "if_expression" || Name == "when_expression") Add(Role::ValueBranching);
	if(Language == Lang::HTML) {
		if(Name == "document" || Name == "element" || Name == "script_element" || Name == "style_element") Add(Role::MarkupContainer);
		if(Name == "start_tag") Add(Role::MarkupOpen);
		if(Name == "end_tag") Add(Role::MarkupClose);
		// 属性の間と閉じの前を折返の候補にする開始タグ（行分割仕様 2.1 の JSX / HTML の属性）
		if(Name == "start_tag" || Name == "self_closing_tag") Add(Role::ExpressionTag);
		if(Name == "text" || Name == "entity" || Name == "raw_text") Add(Role::MarkupText);
	}
	// 末尾の値と語で囲む本体の役割
	if(Language == Lang::Ruby) {
		Roles.reset(static_cast<size_t>(Role::SectionHeader));
		if(Name == "method" || Name == "singleton_method") Add(Role::TailValueFunction);
		static constexpr std::string_view Values[] = {
			"array",
			"binary",
			"call",
			"class_variable",
			"complex",
			"conditional",
			"constant",
			"element_reference",
			"false",
			"float",
			"global_variable",
			"hash",
			"heredoc_beginning",
			"identifier",
			"instance_variable",
			"integer",
			"nil",
			"parenthesized_statements",
			"range",
			"rational",
			"regex",
			"scope_resolution",
			"self",
			"simple_symbol",
			"string",
			"string_array",
			"symbol_array",
			"true",
			"unary"
		};
		if(std::ranges::find(Values, Name) != std::end(Values)) Add(Role::ReturnValue);
		static constexpr std::string_view Blocks[] = {
			"begin",
			"block",
			"case",
			"case_match",
			"class",
			"do_block",
			"for",
			"if",
			"lambda",
			"method",
			"module",
			"singleton_class",
			"singleton_method",
			"unless",
			"until",
			"while"
		};
		static constexpr std::string_view Sections[] = { "do", "else", "elsif", "ensure", "in_clause", "rescue", "then", "when" };
		if(std::ranges::find(Blocks, Name) != std::end(Blocks)) Add(Role::KeywordBlock);
		if(std::ranges::find(Sections, Name) != std::end(Sections)) Add(Role::KeywordSection);
		if(Name == "then" || Name == "do") Add(Role::TransparentSection);
		if(
			Name == "begin" || Name == "class" || Name == "module" || Name == "do_block" || Name == "method" ||
			Name == "singleton_method" || Name == "singleton_class"
		) Add(Role::ExpandedBody);
		if(Name == "case_match") Add(Role::PatternBranch);
		if(Name == "if" || Name == "unless" || Name == "elsif" || Name == "when" || Name == "in_clause") Add(Role::ConditionSection);
		if(Name == "rescue") Add(Role::ExceptionSection);
		if(Name == "ensure") Add(Role::FinalSection);
		if(Name == "body_statement" || Name == "block_body" || Name == "then" || Name == "do") Add(Role::StatementSequence);
	}
	// 役割の返戻
	return Roles;
}

/**
 * 節点名の種別の検索関数
 * @param Name 文法の節点名
 * @param IsNamed 構文の節点か（偽は字句）
 * @return 種別（文法に無ければ無し）
 */
const Core::KindDefinition *GrammarProfile::Find(const std::string_view Name, const bool IsNamed) const {
	const TSSymbol Symbol =
	ts_language_symbol_for_name(Language.TsLang(), Name.data(), static_cast<uint32_t>(Name.size()), IsNamed);
	// 種別の返戻
	return Symbol ? KindsBySymbol.empty() ? &Kinds[Symbol] : KindsBySymbol[Symbol] : nullptr;
}

/**
 * 生成形式の付与関数（整形仕様 3.2, 5.5）
 * 固定字句・返戻・無条件反復の共通形式と，単一文の本体を包む塊等の節点毎の形式を定める
 * @throws FormatLimitExceeded 文法が形式の部品の種別を持たない場合（文法の定義の不具合）
 */
void GrammarProfile::AttachForms() {
	// 共通の固定字句の生成形式
	Core::CanonicalForm Lexemes;
	Lexemes.Operation = Core::FormOperation::GeneratedLexemes;
	Lexemes.Parts = {
		{ Core::FormPartRole::BlockOpen, nullptr, "{" },
		{ Core::FormPartRole::Separator, nullptr, "," },
		{ Core::FormPartRole::Terminator, nullptr, ";" },
		{ Core::FormPartRole::ValueDelimiter, nullptr, ":" },
		{ Core::FormPartRole::BlockClose, nullptr, "}" },
		{ Core::FormPartRole::CallOpen, nullptr, "(" },
		{ Core::FormPartRole::CallClose, nullptr, ")" }
	};
	Rules.GeneratedForms.push_back(std::move(Lexemes));
	// 返戻と反復の形式に使う節点種別の取得
	const Core::KindDefinition *const ReturnDefinition = Find(
		Language == Lang::Kotlin ?
		"jump_expression" :
		Language == Lang::Swift ? "control_transfer_statement" : Language == Lang::Rust ? "return_expression" : "return_statement",
		true
	);
	const Core::KindDefinition *const ReturnStatementDefinition = Find("return_statement", true);
	const Core::KindDefinition *const WhileStatementDefinition = Find("while_statement", true);
	const std::vector<std::string_view> &ReturnWords = Rules.Words[static_cast<size_t>(Core::Word::Return)];
	// 引数を持たない返戻の形式
	if(ReturnDefinition && !ReturnWords.empty()) {
		Core::CanonicalForm Return;
		Return.Operation = Core::FormOperation::ArglessReturn;
		const bool HasWrapper = Language == Lang::Rust && Rules.ExpressionStatementDefinition != nullptr;
		Return.Parts.push_back(
			{
				Core::FormPartRole::Result,
				HasWrapper ? Rules.ExpressionStatementDefinition : ReturnDefinition,
				{},
				Core::FormPartRole::Result,
				Core::ChildRole::None,
				true
			}
		);
		if(HasWrapper) {
			Return.Parts.push_back(
				{ Core::FormPartRole::Arguments, ReturnDefinition, {}, Core::FormPartRole::Result, Core::ChildRole::None, true }
			);
		}
		Return.Parts.push_back(
			{
				Core::FormPartRole::Keyword,
				nullptr,
				ReturnWords.front(),
				HasWrapper ? Core::FormPartRole::Arguments : Core::FormPartRole::Result
			}
		);
		if(
			HasWrapper || Language.IsCFamily() || Language.IsJsTs() || Language == Lang::CSharp || Language == Lang::Java ||
			Language == Lang::PHP
		) Return.Parts.push_back({ Core::FormPartRole::Terminator, nullptr, ";" });
		Rules.GeneratedForms.push_back(std::move(Return));
	}
	// 零値を返す形式
	if(ReturnStatementDefinition && !ReturnWords.empty()) {
		Core::CanonicalForm Return;
		Return.Operation = Core::FormOperation::ReturnWithZero;
		Return.Parts = {
			{ Core::FormPartRole::Result, ReturnStatementDefinition, {}, Core::FormPartRole::Result, Core::ChildRole::None, true },
			{ Core::FormPartRole::Keyword, nullptr, ReturnWords.front() },
			{
				Core::FormPartRole::IntegerValue,
				Find("number_literal", true),
				"0",
				Core::FormPartRole::Result,
				Core::ChildRole::None,
				true
			},
			{ Core::FormPartRole::Terminator, nullptr, ";" }
		};
		Rules.GeneratedForms.push_back(std::move(Return));
	}
	// 無条件反復の形式
	if(WhileStatementDefinition && !Rules.Words[static_cast<size_t>(Core::Word::While)].empty()) {
		Core::CanonicalForm Loop;
		Loop.Operation = Core::FormOperation::UnconditionalLoop;
		Loop.Parts.push_back(
			{ Core::FormPartRole::Result, WhileStatementDefinition, {}, Core::FormPartRole::Result, Core::ChildRole::None, true }
		);
		Loop.Parts.push_back({ Core::FormPartRole::Keyword, nullptr, Rules.Words[static_cast<size_t>(Core::Word::While)].front() });
		if(Language == Lang::CSharp) {
			Loop.Parts.push_back({ Core::FormPartRole::Open, nullptr, "(" });
			Loop.Parts.push_back(
				{
					Core::FormPartRole::Condition,
					Find("boolean_literal", true),
					{},
					Core::FormPartRole::Result,
					Core::ChildRole::Condition,
					true
				}
			);
			Loop.Parts.push_back(
				{
					Core::FormPartRole::TrueValue,
					nullptr,
					Rules.Words[static_cast<size_t>(Core::Word::True)].front(),
					Core::FormPartRole::Condition
				}
			);
			Loop.Parts.push_back({ Core::FormPartRole::Close, nullptr, ")" });
		} else {
			const Core::KindDefinition *const Condition =
			Language == Lang::Cpp ? Find("condition_clause", true) : Find("parenthesized_expression", true);
			Loop.Parts.push_back(
				{ Core::FormPartRole::Condition, Condition, {}, Core::FormPartRole::Result, Core::ChildRole::Condition, true }
			);
			Loop.Parts.push_back({ Core::FormPartRole::Open, nullptr, "(", Core::FormPartRole::Condition });
			Loop.Parts.push_back(
				{
					Core::FormPartRole::TrueValue,
					Rules.TrueDefinition,
					Rules.Words[static_cast<size_t>(Core::Word::True)].front(),
					Core::FormPartRole::Condition,
					Language == Lang::Cpp ? Core::ChildRole::Value : Core::ChildRole::None,
					true
				}
			);
			Loop.Parts.push_back(
				{
					Core::FormPartRole::IntegerValue,
					Find("number_literal", true),
					"1",
					Core::FormPartRole::Condition,
					Language == Lang::Cpp ? Core::ChildRole::Value : Core::ChildRole::None,
					true
				}
			);
			Loop.Parts.push_back({ Core::FormPartRole::Close, nullptr, ")", Core::FormPartRole::Condition });
		}
		Loop.Parts.push_back({ Core::FormPartRole::Body, nullptr, {}, Core::FormPartRole::Result, Core::ChildRole::Body });
		Rules.GeneratedForms.push_back(std::move(Loop));
	}
	using Core::Role;
	// 後置条件と末尾返戻の形式
	if(Language == Lang::Ruby) {
		for(const std::string_view Name : { "if", "unless", "until", "while" }) {
			Core::CanonicalForm Modifier;
			Modifier.Operation = Core::FormOperation::PostfixCondition;
			Modifier.Parts = { { Core::FormPartRole::Result, Find(std::string(Name) + "_modifier", true) } };
			for(TSSymbol Symbol = 0; Symbol < Kinds.size(); ++Symbol) {
				if(SymbolNames[Symbol] == Name && Kinds[Symbol].IsNamed) FormDefinitions[Symbol].push_back(Modifier);
			}
		}
		Core::CanonicalForm ReturnForm;
		ReturnForm.Operation = Core::FormOperation::ExplicitTailReturn;
		ReturnForm.Parts = {
			{ Core::FormPartRole::Result, Find("return", true) },
			{ Core::FormPartRole::Keyword, Find("return", false), "return" },
			{ Core::FormPartRole::Arguments, Find("argument_list", true) },
			{ Core::FormPartRole::Open, Find("(", false), "(" },
			{ Core::FormPartRole::Close, Find(")", false), ")" }
		};
		for(TSSymbol Symbol = 0; Symbol < Kinds.size(); ++Symbol) {
			if(Kinds[Symbol].Roles.test(static_cast<size_t>(Role::TailValueFunction))) FormDefinitions[Symbol].push_back(ReturnForm);
		}
	}
	// 継続式を包む形式
	if(Language == Lang::Python) for(size_t Symbol = 0; Symbol < Kinds.size(); ++Symbol) {
		const Core::KindDefinition &Kind = Kinds[Symbol];
		if(
			!Kind.Roles.test(static_cast<size_t>(Role::ContinuationExpression)) && SymbolNames[Symbol] != "tuple" &&
			SymbolNames[Symbol] != "tuple_pattern"
		) continue;
		const std::string_view RawName = SymbolNames[Symbol];
		const std::string_view Name = RawName == "expression_list" || RawName == "tuple" || RawName == "tuple_expression" ?
		"tuple" :
		RawName == "pattern_list" || RawName == "tuple_pattern" ? "tuple_pattern" : "parenthesized_expression";
		Core::CanonicalForm Form { Core::FormOperation::Continuation, { { Core::FormPartRole::Result, Find(Name, true) } } };
		Form.ApplicableRoles.set(static_cast<size_t>(Role::ContinuationExpression));
		if(Kind.Roles.test(static_cast<size_t>(Role::TypeSyntax))) {
			Form.Parts.push_back({ Core::FormPartRole::Body, Find("subscript", true) });
		}
		FormDefinitions[Symbol].push_back(std::move(Form));
	}
	// 空の注釈引数を省く形式
	if(Language == Lang::Java) {
		const Core::KindDefinition *const Annotation = Find("annotation", true), *const Marker = Find("marker_annotation", true);
		if(!Annotation || !Marker) {
			throw FormatLimitExceeded("the grammar has no kind for a marker annotation", FailureReason::FormatterDefect);
		}
		Core::CanonicalForm MarkerForm { Core::FormOperation::OmitEmptyArguments, { { Core::FormPartRole::Result, Marker } } };
		MarkerForm.ApplicableRoles.set(static_cast<size_t>(Role::AnnotationParts));
		MarkerForm.Condition = Core::FormCondition::EmptyArguments;
		FormDefinitions[static_cast<TSSymbol>(Annotation - Kinds.data())].push_back(std::move(MarkerForm));
	}
	// 末尾カンマの形式
	if(Language == Lang::Go) {
		Core::CanonicalForm CommaForm {
			Core::FormOperation::TrailingComma,
			{ { Core::FormPartRole::LastElement, nullptr }, { Core::FormPartRole::Close, nullptr } },
			","
		};
		CommaForm.ApplicableRoles.set(static_cast<size_t>(Role::TrailingCommaHost));
		CommaForm.Condition = Core::FormCondition::BrokenListWithLastElement;
		CommaForm.ClosingLexemes = ")]}";
		for(size_t Symbol = 0; Symbol < Kinds.size(); ++Symbol) {
			if(Kinds[Symbol].Roles.test(static_cast<size_t>(Role::TrailingCommaHost))) FormDefinitions[Symbol].push_back(CommaForm);
		}
	}
	// 本体を包む塊と其の中の文の並びの節点名
	std::string_view BlockName, SequenceName;
	if(Language == Lang::CSharp || Language == Lang::Java) BlockName = "block";
	else if(Language.IsJsTs()) BlockName = "statement_block";
	else if(Language.IsCFamily() || Language == Lang::PHP) BlockName = "compound_statement";
	else if(Language == Lang::Kotlin) {
		BlockName = "control_structure_body";
		SequenceName = "statements";
	}
	// 波括弧を省略出来ない文法の終了
	if(BlockName.empty()) return;
	Core::CanonicalForm Form {
		Core::FormOperation::BraceBody,
		{
			{ Core::FormPartRole::Block, Find(BlockName, true) },
			{ Core::FormPartRole::Open, Find("{", false), "{" },
			{ Core::FormPartRole::Body, nullptr },
			{ Core::FormPartRole::Close, Find("}", false), "}" }
		}
	};
	if(!SequenceName.empty()) Form.Parts.push_back({ Core::FormPartRole::Statements, Find(SequenceName, true) });
	for(const Role OwnerRole : { Role::BodyOwner, Role::ElseClause, Role::If, Role::ValueBranching }) {
		Form.ApplicableRoles.set(static_cast<size_t>(OwnerRole));
	}
	for(const Core::FormPart &Part : Form.Parts) if(Part.Role != Core::FormPartRole::Body && !Part.Definition) {
		throw FormatLimitExceeded("the grammar has no kind for a brace body", FailureReason::FormatterDefect);
	}
	for(size_t Symbol = 0; Symbol < Kinds.size(); ++Symbol) {
		const Core::KindDefinition &Kind = Kinds[Symbol];
		const bool IsOwner =
		Kind.Roles.test(static_cast<size_t>(Role::BodyOwner)) || Kind.Roles.test(static_cast<size_t>(Role::ElseClause)) ||
		Kind.Roles.test(static_cast<size_t>(Role::If)) || Kind.Roles.test(static_cast<size_t>(Role::ValueBranching));
		if(IsOwner) FormDefinitions[Symbol].push_back(Form);
	}
	// 終了
	return;
}

/**
 * 入力側だけが区別する場の役割の中核の子の役割への対応関数
 * 同じ中核の役割へ写る場の名前（`function` と `method` 等）を入力側の照合では区別し，中核へは共通の役割だけを渡す
 * @param Role 場の役割
 * @return 中核の子の役割
 */
Core::ChildRole GrammarProfile::CommonChildRole(const Core::ChildRole Role) {
	switch(Role) {
	case Core::ChildRole::ArgumentField:
	case Core::ChildRole::BlockField:
	case Core::ChildRole::DirectiveField:
		// 共通の役割を持たない場の返戻
		return Core::ChildRole::Other;
	case Core::ChildRole::FunctionField:
	case Core::ChildRole::MethodField:
		// 呼ぶ関数の場の返戻
		return Core::ChildRole::Function;
	case Core::ChildRole::ObjectField:
	case Core::ChildRole::ReceiverField:
		// 受け手の場の返戻
		return Core::ChildRole::Object;
	case Core::ChildRole::OperatorField:
		// 演算子の場の返戻
		return Core::ChildRole::Operator;
	case Core::ChildRole::ParametersField:
		// 仮引数の場の返戻
		return Core::ChildRole::Parameters;
	default:
		// 其の儘中核の役割である場の返戻
		return Role;
	}
}

/**
 * 文法の場に対応する子の役割の取得関数
 * @param Field 文法の場の識別子
 * @return 共通の子の役割（未使用の場は無し）
 */
Core::ChildRole GrammarProfile::ChildRoleOf(const TSFieldId Field) const {
	return Field < FieldRoles.size() ? FieldRoles[Field] : Core::ChildRole::None;
}

/**
 * 文法の場に対応する中核の子の役割の印の取得関数
 * @param Field 場の識別子
 * @return 中核の子の役割の印（場でなければ０）
 */
uint64_t GrammarProfile::FieldBits(const TSFieldId Field) const {
	const Core::ChildRole Role = ChildRoleOf(Field);
	// 場でない子と，中核の子の役割の印の返戻
	return Role == Core::ChildRole::None ? 0 : uint64_t{ 1 } << static_cast<uint8_t>(CommonChildRole(Role));
}

/**
 * コンストラクタ
 * 文法の全ての記号の役割と配置を導出し，同じ性質の共通種別を共有する（整形仕様 4.3）
 * @param Language 対象言語と方言
 */
GrammarProfile::GrammarProfile(const Lang Language) : Language(Language) {
	// 静的検査
	Rules.FunctionCase = Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::Java || Language == Lang::Kotlin ?
	Core::NameCase::UpperInitial :
	Language == Lang::Rust || Language == Lang::Ruby || Language == Lang::Python ?
	Core::NameCase::NoUppercase :
	Language == Lang::Swift || Language == Lang::PHP || Language.IsJsTs() ?
	Core::NameCase::LowerInitial :
	Core::NameCase::Unrestricted;
	Rules.FunctionCaseByFirst = Language == Lang::Go;
	Rules.BooleanTypeNames = Language == Lang::Swift ? std::vector<std::string_view>{ "Bool" } : Language == Lang::TypeScript ?
	std::vector<std::string_view>{ "boolean" } :
	Language == Lang::Python ? std::vector<std::string_view>{ "bool" } : Language == Lang::Kotlin || Language == Lang::Java ?
	std::vector<std::string_view>{ "Boolean", "boolean" } :
	std::vector<std::string_view>{ "bool", "Bool", "_Bool" };
	Rules.BooleanQueryPrefixes =
	Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::Java || Language == Lang::Kotlin ?
	std::vector<std::string_view>{ "Is", "Has", "Can", "Should" } :
	Language == Lang::Go ?
	std::vector<std::string_view>{ "Is", "Has", "Can", "Should", "is", "has", "can", "should" } :
	std::vector<std::string_view>{ "is", "has", "can", "should" };
	if(Language == Lang::Ruby) {
		Rules.BooleanQuerySuffix = "?";
		Rules.DestructiveMethodSuffix = "!";
		Rules.SetterNameSuffix = "=";
		Rules.InstanceStatePrefix = "@";
	}
	Rules.GetterPrefixes = Rules.FunctionCase == Core::NameCase::UpperInitial ?
	std::vector<std::string_view>{ "Get" } :
	Rules.FunctionCase == Core::NameCase::NoUppercase ?
	std::vector<std::string_view>{ "get_" } :
	Rules.FunctionCaseByFirst ? std::vector<std::string_view>{ "Get", "get" } : std::vector<std::string_view>{ "get" };
	Rules.SetterPrefixes = Rules.FunctionCase == Core::NameCase::UpperInitial ?
	std::vector<std::string_view>{ "Set" } :
	Rules.FunctionCase == Core::NameCase::NoUppercase ?
	std::vector<std::string_view>{ "set_" } :
	Rules.FunctionCaseByFirst ? std::vector<std::string_view>{ "Set", "set" } : std::vector<std::string_view>{ "set" };
	Rules.TypeAnnotationObserved = Rules.HasReceiverParameter = Language == Lang::Python;
	Rules.DetailedMemberOrder = Language == Lang::Cpp;
	if(Rules.DetailedMemberOrder) {
		Rules.MemberStaticWord = "static";
		Rules.MemberVirtualWord = "virtual";
	}
	Rules.DocumentationConstraintHeadings = { "制約：", "Constraints:", "Preconditions:" };
	Rules.DocumentationComplexityHeadings = { "計算量：", "Complexity:" };
	Rules.InferredType = Language == Lang::Cpp ?
	Core::InferredTypeNotation::Placeholder :
	Language == Lang::CSharp ? Core::InferredTypeNotation::Implicit : Language == Lang::Java ?
	Core::InferredTypeNotation::Named :
	Language == Lang::TypeScript ? Core::InferredTypeNotation::DynamicTop : Core::InferredTypeNotation::None;
	if(Language.IsJsTs()) Rules.BigIntegerSuffix = "n";
	Rules.Notation.Documentation = Language == Lang::CSharp ? Core::DocumentationStyle::XmlTag : Language == Lang::Rust ?
	Core::DocumentationStyle::MarkdownHeading :
	Language == Lang::Swift ? Core::DocumentationStyle::Callout : Language == Lang::Go ?
	Core::DocumentationStyle::Prose :
	Language == Lang::Python ? Core::DocumentationStyle::Docstring : Core::DocumentationStyle::AtTag;
	Rules.DocumentationForm = Language.IsCFamily() ?
	"Doxygen block-comment syntax" :
	Language == Lang::CSharp ? "XML block-comment syntax" : Language == Lang::Java ?
	"Javadoc block-comment syntax" :
	Language == Lang::Go ? "Go declaration documentation" : Language == Lang::Rust ?
	"outer Rust block documentation" :
	Language == Lang::Kotlin ? "KDoc block-comment syntax" : Language == Lang::Swift ?
	"Swift block documentation" :
	Language == Lang::PHP ? "PHPDoc block-comment syntax" : Language.IsJsTs() ?
	"JSDoc block-comment syntax" :
	Language == Lang::Ruby ? "YARD line documentation" : Language == Lang::Python ? "a triple-double-quoted docstring" : "";
	if(Language == Lang::C || Language == Lang::Java || Language == Lang::Kotlin) Rules.FixedFunctionNames = { "main" };
	// C++ は起動点と，言語仕様がコルーチンの約束・待機の型へ要求する名前
	if(Language == Lang::Cpp) {
		Rules.FixedFunctionNames = {
			"await_ready",
			"await_resume",
			"await_suspend",
			"await_transform",
			"final_suspend",
			"get_return_object",
			"get_return_object_on_allocation_failure",
			"initial_suspend",
			"main",
			"promise_type",
			"return_value",
			"return_void",
			"unhandled_exception",
			"yield_value"
		};
	}
	if(Language == Lang::PHP) Rules.FixedFunctionPrefix = "__";
	if(Language == Lang::Java) Rules.FixedNameModifiers = { "Override" };
	if(Language == Lang::Kotlin) Rules.FixedNameModifiers = { "operator", "override" };
	Rules.CountNames = Language.IsJsTs() ?
	std::vector<std::string_view>{ "length" } :
	Language == Lang::Ruby ? std::vector<std::string_view>{ "count", "length", "size" } : Language == Lang::Python ?
	std::vector<std::string_view>{ "len" } :
	std::vector<std::string_view>{ "count", "mb_strlen", "sizeof", "strlen" };
	Rules.CountNameRole = Language.IsJsTs() ? Core::ChildRole::Other : Core::ChildRole::Function;
	Rules.Notation.FailureTag = Language == Lang::Ruby ? "@raise" : "@throws";
	Rules.DefaultIntegerType = Language == Lang::Java ? "int" : "";
	Rules.LongBits = Language == Lang::CSharp || Language == Lang::Java ? 64 : 32;
	// 表示幅４・２のインデントを推奨する言語のネストの段数の上限（幅の狭い言語は同じ桁数へ倍の段が収まる）
	constexpr size_t WideIndentNestingLimit = 32, NarrowIndentNestingLimit = 64;
	Rules.NestingLimit =
	Language.IsJsTs() || Language == Lang::Ruby || Language == Lang::JSON || Language == Lang::HTML || Language == Lang::CSS ?
	NarrowIndentNestingLimit :
	WideIndentNestingLimit;
	Rules.ByteIsSigned = Language == Lang::Java;
	Rules.IntegerTypeNames = {
		{ "Byte", 8, true },
		{ "Int", 32, true },
		{ "Int16", 16, true },
		{ "Int32", 32, true },
		{ "Int64", 64, true },
		{ "Int8", 8, true },
		{ "Long", 64, true },
		{ "Short", 16, true },
		{ "UByte", 8, false },
		{ "UInt", 32, false },
		{ "UInt16", 16, false },
		{ "UInt32", 32, false },
		{ "UInt64", 64, false },
		{ "UInt8", 8, false },
		{ "ULong", 64, false },
		{ "UShort", 16, false },
		{ "i128", 128, true },
		{ "i16", 16, true },
		{ "i32", 32, true },
		{ "i64", 64, true },
		{ "i8", 8, true },
		{ "int", 32, true },
		{ "int16", 16, true },
		{ "int16_t", 16, true },
		{ "int32", 32, true },
		{ "int32_t", 32, true },
		{ "int64", 64, true },
		{ "int64_t", 64, true },
		{ "int8", 8, true },
		{ "int8_t", 8, true },
		{ "isize", 16, true },
		{ "long", 32, true },
		{ "long int", 32, true },
		{ "long long", 64, true },
		{ "long long int", 64, true },
		{ "ptrdiff_t", 32, true },
		{ "sbyte", 8, true },
		{ "short", 16, true },
		{ "short int", 16, true },
		{ "signed", 32, true },
		{ "signed char", 8, true },
		{ "size_t", 16, false },
		{ "u128", 128, false },
		{ "u16", 16, false },
		{ "u32", 32, false },
		{ "u64", 64, false },
		{ "u8", 8, false },
		{ "uint", 32, false },
		{ "uint16", 16, false },
		{ "uint16_t", 16, false },
		{ "uint32", 32, false },
		{ "uint32_t", 32, false },
		{ "uint64", 64, false },
		{ "uint64_t", 64, false },
		{ "uint8", 8, false },
		{ "uint8_t", 8, false },
		{ "ulong", 64, false },
		{ "unsigned", 32, false },
		{ "unsigned char", 8, false },
		{ "unsigned int", 32, false },
		{ "unsigned long", 32, false },
		{ "unsigned long long", 64, false },
		{ "unsigned short", 16, false },
		{ "ushort", 16, false },
		{ "usize", 16, false }
	};
	Rules.FloatingTypeNames = {
		"CGFloat",
		"Decimal",
		"Double",
		"Float",
		"Float16",
		"Float32",
		"Float64",
		"Float80",
		"decimal",
		"double",
		"f32",
		"f64",
		"float",
		"float32",
		"float64",
		"long double"
	};
	Rules.CharacterTypeNames = { "Char", "Character", "char", "char16_t", "char32_t", "char8_t", "rune", "wchar_t" };
	Rules.HasLeadingZeroOctal =
	Language.IsCFamily() || Language.IsJsTs() || Language == Lang::Java || Language == Lang::Go || Language == Lang::PHP;
	Rules.RequiresKnownInteger = Language.IsJsTs() || Language == Lang::PHP || Language == Lang::Ruby || Language == Lang::Python;
	Rules.OrderMembers = Language == Lang::Cpp || Language == Lang::CSharp || Language == Lang::Java || Language == Lang::PHP;
	Rules.AccessOrder = Language == Lang::CSharp ?
	std::vector<std::string_view>{ "private", "private protected", "protected", "internal", "protected internal", "public" } :
	Language == Lang::Java ?
	std::vector<std::string_view>{ "private", "package-private", "protected", "public" } :
	Language == Lang::Kotlin ?
	std::vector<std::string_view>{ "private", "protected", "internal", "public" } :
	Language == Lang::Swift ?
	std::vector<std::string_view>{ "private", "fileprivate", "internal", "package", "public", "open" } :
	Language == Lang::Cpp || Language == Lang::PHP || Language == Lang::TypeScript ?
	std::vector<std::string_view>{ "private", "protected", "public" } :
	std::vector<std::string_view>{};
	Rules.ImplicitAccess = Language == Lang::Java ?
	"package-private" :
	Language == Lang::Swift ? "internal" : Language == Lang::Cpp || Language == Lang::CSharp ? "private" : "public";
	Rules.RequireExplicitPrivate = Language == Lang::Cpp;
	Rules.RequireExplicitInternal = Language == Lang::Swift;
	Rules.HasAccessSections = Language == Lang::Cpp;
	Rules.HasModifierGroups = Language == Lang::Java;
	Rules.HasReturnLabels = Language == Lang::Kotlin;
	Rules.HasReturnComments = Language != Lang::CSS;
	Rules.HasStrictEquality = Language.IsJsTs();
	Rules.AllowsNumericTruthiness = Language.IsCFamily() || Language.IsJsTs() || Language == Lang::Python;
	Rules.PrefersBooleanXorAssignment =
	Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::Java || Language == Lang::Rust ||
	Language == Lang::Ruby || Language == Lang::Python;
	Rules.PrefersUnitUpdate =
	Language.IsCFamily() || Language.IsJsTs() || Language == Lang::CSharp || Language == Lang::Java || Language == Lang::Go ||
	Language == Lang::Kotlin || Language == Lang::PHP;
	Rules.PrefersPrefixUpdate = Rules.PrefersUnitUpdate && Language != Lang::Go;
	// 演算子の各欄を位置で初期化する為，欄の宣言順を保つ
	Rules.Operators =
	{ "=", "==", "!=", "===", "!==", "&&", "||", "!", "+", "-", "+=", "-=", "++", "--", ">=", "<=", ">", "<", "." };
	// 論理演算を語で綴る記法
	if(Language == Lang::Python) {
		Rules.Operators.LogicalAnd = "and";
		Rules.Operators.LogicalOr = "or";
		Rules.Operators.LogicalNot = "not";
	}
	Rules.CatchAllBindings = Language.IsJsTs();
	Rules.CatchBindingPattern = Language == Lang::Swift;
	Rules.CatchExceptionList = Language == Lang::Ruby;
	Rules.CatchFilter = Language == Lang::CSharp;
	Rules.Notation.ReviewMarkdownLine = Language == Lang::Java;
	Rules.Notation.HasMarkupComments = Language == Lang::HTML;
	Rules.MarkupCommentOpen = Language == Lang::HTML ? "<!--" : "";
	Rules.MarkupCommentClose = Language == Lang::HTML ? "-->" : "";
	if(Language == Lang::HTML) {
		Reading.PreserveSpaceValue = "preserve";
		Reading.NativeNamespaceValue = "http://www.w3.org/1999/xhtml";
		Reading.EmbeddedStyleType = "text/css";
		Reading.EmbeddedTypeAttribute = "type";
		Reading.AttributeValueEdges = "\"' \t";
		Reading.EmbeddedScriptLanguage = Lang::JavaScript;
		Reading.EmbeddedStyleLanguage = Lang::CSS;
		Reading.EmbeddedDataLanguage = Lang::JSON;
		Reading.EmbeddedScriptTypes = {
			"application/ecmascript",
			"application/javascript",
			"application/x-ecmascript",
			"application/x-javascript",
			"module",
			"text/ecmascript",
			"text/javascript",
			"text/javascript1.0",
			"text/javascript1.1",
			"text/javascript1.2",
			"text/javascript1.3",
			"text/javascript1.4",
			"text/javascript1.5",
			"text/jscript",
			"text/livescript",
			"text/x-ecmascript",
			"text/x-javascript"
		};
		Reading.EmbeddedDataTypes = { "application/json", "importmap", "speculationrules" };
		Reading.StructuredDataTypePrefix = "application/";
		Reading.StructuredDataTypeSuffix = "+json";
	}
	// 文法の場の共通役割の定義
	const TSLanguage *const Grammar = Language.TsLang();
	static constexpr std::pair<std::string_view, Core::ChildRole> ChildRoles[] {
		{ "alias", Core::ChildRole::Alias },
		{ "alternative", Core::ChildRole::Alternative },
		{ "argument", Core::ChildRole::ArgumentField },
		{ "arguments", Core::ChildRole::Arguments },
		{ "block", Core::ChildRole::BlockField },
		{ "body", Core::ChildRole::Body },
		{ "communication", Core::ChildRole::Communication },
		{ "condition", Core::ChildRole::Condition },
		{ "consequence", Core::ChildRole::Consequence },
		{ "declarator", Core::ChildRole::Declarator },
		{ "directive", Core::ChildRole::DirectiveField },
		{ "exceptions", Core::ChildRole::Exceptions },
		{ "function", Core::ChildRole::FunctionField },
		{ "guard", Core::ChildRole::Guard },
		{ "if_true", Core::ChildRole::IfTrue },
		{ "increment", Core::ChildRole::Increment },
		{ "left", Core::ChildRole::Left },
		{ "macro", Core::ChildRole::Macro },
		{ "method", Core::ChildRole::MethodField },
		{ "name", Core::ChildRole::Name },
		{ "object", Core::ChildRole::ObjectField },
		{ "op", Core::ChildRole::Operator },
		{ "operand", Core::ChildRole::Operand },
		{ "operation", Core::ChildRole::Operator },
		{ "operator", Core::ChildRole::OperatorField },
		{ "parameter", Core::ChildRole::Parameters },
		{ "parameters", Core::ChildRole::ParametersField },
		{ "path", Core::ChildRole::Path },
		{ "pattern", Core::ChildRole::Pattern },
		{ "receiver", Core::ChildRole::ReceiverField },
		{ "result", Core::ChildRole::Result },
		{ "return_type", Core::ChildRole::ReturnType },
		{ "returns", Core::ChildRole::ReturnType },
		{ "right", Core::ChildRole::Right },
		{ "superclass", Core::ChildRole::Superclass },
		{ "target", Core::ChildRole::Target },
		{ "type", Core::ChildRole::Type },
		{ "update", Core::ChildRole::Update },
		{ "value", Core::ChildRole::Value },
		{ "variable", Core::ChildRole::Variable }
	};
	// 共通の役割を持たない場も，子が自身の場を持つ事実（隠れた規則の場を受け継がない）を保つ為に其の他の役割へ写す
	FieldRoles.resize(ts_language_field_count(Grammar) + 1, Core::ChildRole::Other);
	FieldRoles.front() = Core::ChildRole::None;
	for(TSFieldId Field = 1; Field < FieldRoles.size(); ++Field) {
		const std::string_view Name = ts_language_field_name_for_id(Grammar, Field);
		for(const auto &[FieldName, ChildRole] : ChildRoles) if(Name == FieldName) {
			FieldRoles[Field] = ChildRole;
			break;
		}
		ChildRoleMask |= FieldBits(Field);
		const size_t RoleIndex = static_cast<size_t>(FieldRoles[Field]);
		if(FieldsByRole.size() <= RoleIndex) FieldsByRole.resize(RoleIndex + 1);
		FieldsByRole[RoleIndex].push_back(Field);
	}
	// 記号毎の定義領域の初期化
	const uint32_t Count = ts_language_symbol_count(Grammar);
	Kinds.resize(Count);
	SymbolNames.resize(Count);
	NameSymbols.resize(Count);
	FormDefinitions.resize(Count);
	std::unordered_map<std::string_view, TSSymbol> FirstByName;
	// 公開節点型の名前と名前付か否かを，解析器の記号から独立した空行比較の値へ投影する
	std::unordered_map<std::string_view, std::array<uint32_t, 2>> BlankKeys;
	uint32_t NextBlankKey = 0;
	for(TSSymbol Symbol = 0; Symbol < Count; ++Symbol) {
		Core::KindDefinition &Kind = Kinds[Symbol];
		SymbolNames[Symbol] = ts_language_symbol_name(Grammar, Symbol);
		NameSymbols[Symbol] = FirstByName.try_emplace(SymbolNames[Symbol], Symbol).first->second;
		const TSSymbolType SymbolType = ts_language_symbol_type(Grammar, Symbol);
		Kind.IsNamed = SymbolType == TSSymbolTypeRegular;
		Kind.Roles = DeriveRoles(Language, SymbolNames[Symbol], Kind.IsNamed);
		uint32_t &BlankKey = BlankKeys[SymbolNames[Symbol]][Kind.IsNamed];
		if(!BlankKey) BlankKey = ++NextBlankKey;
		Kind.BlankKey = BlankKey;
		// 生成する字句の種別は字面毎に最初の見える字句の記号で引き，字面の解析器の記号は解析器が節点に公開する記号を字面毎に一度だけ引く
		if(SymbolType == TSSymbolTypeAnonymous) {
			Rules.TokenDefinitions.try_emplace(SymbolNames[Symbol], &Kind);
			if(const std::string_view Name = SymbolNames[Symbol]; !TokenSymbols.contains(Name)) {
				TokenSymbols.emplace(Name, ts_language_symbol_for_name(Grammar, Name.data(), static_cast<uint32_t>(Name.size()), false));
			}
		}
	}
	// 位置を観測する構文の節点名を文法の記号へ一度だけ対応付ける
	struct ObservationName {
		std::string_view Kind; // 節点名
		std::string_view Name; // 名前の綴り
		bool IsCaseInsensitive = false; // 綴りの大小を区別しないか
		bool ObservesColumn = false; // 桁位置も観測するか
		std::string_view CallerKind {}; // 呼出側の位置を観測する場合の呼出の節点名
	};
	static constexpr std::pair<Lang::ID, ObservationName> NamedObservations[] = {
		{ Lang::C, { "identifier", "__LINE__" } },
		{ Lang::C, { "call_expression", "__builtin_COLUMN", false, true } },
		{ Lang::C, { "call_expression", "__builtin_LINE" } },
		{ Lang::Cpp, { "identifier", "__LINE__" } },
		{ Lang::Cpp, { "call_expression", "__builtin_COLUMN", false, true } },
		{ Lang::Cpp, { "call_expression", "__builtin_LINE" } },
		{ Lang::Cpp, { "call_expression", "source_location::current", false, true } },
		{ Lang::CSharp, { "attribute", "CallerLineNumber", false, false, "invocation_expression" } },
		{ Lang::CSharp, { "attribute", "CallerLineNumberAttribute", false, false, "invocation_expression" } },
		{ Lang::Rust, { "macro_invocation", "column", false, true } },
		{ Lang::Rust, { "macro_invocation", "line" } },
		{ Lang::Swift, { "special_literal", "#column", false, true } },
		{ Lang::Swift, { "special_literal", "#line" } },
		{ Lang::PHP, { "name", "__LINE__", true } },
		{ Lang::Ruby, { "identifier", "__LINE__" } }
	};
	for(const auto &[Target, Entry] : NamedObservations) {
		if(Language.Id != Target) continue;
		const TSSymbol Symbol = ts_language_symbol_for_name(Grammar, Entry.Kind.data(), static_cast<uint32_t>(Entry.Kind.size()), true);
		std::optional<TSSymbol> Caller;
		if(!Entry.CallerKind.empty()) {
			Caller = ts_language_symbol_for_name(Grammar, Entry.CallerKind.data(), static_cast<uint32_t>(Entry.CallerKind.size()), true);
		}
		Observations.push_back({ Symbol, Entry.Name, Entry.IsCaseInsensitive, Entry.ObservesColumn, Caller });
	}
	// 誤り区画の種別の準備
	ErrorKind.IsNamed = true;
	ErrorKind.Roles = DeriveRoles(Language, "ERROR", true);
	ErrorKind.BlankKey = ++NextBlankKey;
	// 本体の外の地の文は投影が加える逐語の子の為，文法の地の文の役割を持たせず本体の配置から外す（整形仕様 4.4）
	OuterTextKind.IsNamed = true;
	OuterTextKind.Roles.set(static_cast<size_t>(Core::Role::OuterText));
	OuterTextKind.BlankKey = ++NextBlankKey;
	TrailingDataKind.IsNamed = true;
	TrailingDataKind.Roles.set(static_cast<size_t>(Core::Role::OuterText));
	TrailingDataKind.Roles.set(static_cast<size_t>(Core::Role::TrailingDataSection));
	TrailingDataKind.BlankKey = ++NextBlankKey;
	// 正規化の規則・制御の流れ・署名・コメント
	// 使用可能な記法（井桁の行コメントを持つ言語，原文の先頭の `#!` を読む言語，文書の囲みを持つ言語，処理系が定める名前付の指示記法，整形仕様 3.2, 4.5）
	// 開始記号の最初の一致を採る為，重なる記法の登録順を保つ
	const bool HasHashComment = Language == Lang::PHP || Language == Lang::Ruby || Language == Lang::Python;
	const bool HasBlockComment = Language != Lang::Ruby && Language != Lang::Python && Language != Lang::HTML;
	std::vector<Core::AvailableNotation> &Available = Rules.Notation.AvailableNotations;
	if(HasBlockComment && (Language != Lang::CSS || Language.IsScss)) Available.push_back({ CommentNotation::SlashLine, false });
	if(HasHashComment) Available.push_back({ CommentNotation::HashLine, false });
	if(HasHashComment || Language.IsJsTs() || Language == Lang::Rust) Available.push_back({ CommentNotation::HashBangLine, false });
	if(HasBlockComment) Available.push_back({ CommentNotation::Block, false });
	if(HasBlockComment && Language != Lang::Go && Language != Lang::JSON && Language != Lang::CSS) {
		Available.push_back({ CommentNotation::DoubleStarBlock, false });
	}
	if(Language.IsCFamily() || Language == Lang::Rust || Language.IsScss) {
		Available.push_back({ CommentNotation::StarBangBlock, false });
	}
	if(Language.IsCFamily() || Language == Lang::Rust) Available.push_back({ CommentNotation::SlashBangLine, false });
	if(
		Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::Java || Language == Lang::Rust ||
		Language == Lang::Swift || Language == Lang::TypeScript
	) Available.push_back({ CommentNotation::TripleSlashLine, false });
	if(Language == Lang::Ruby) {
		Available.push_back({ CommentNotation::HashColonLine, false });
		Available.push_back({ CommentNotation::RubyEmbeddedDocument, false });
	}
	Rules.Notation.IsBlockCommentNested = Language == Lang::Rust || Language == Lang::Kotlin || Language == Lang::Swift;
	Reading.HasCommentDocumentContent = Language == Lang::HTML;
	Reading.HasDirectiveHeader =
	Language == Lang::Go || Language == Lang::Ruby || Language == Lang::Python || Language.IsCFamily() ||
	Language == Lang::TypeScript;
	Reading.HasSingleLineEncodingHeader = Language == Lang::Ruby;
	Reading.HasConditionalSecondLineEncoding = Language == Lang::Python;
	Reading.HasOutputCommentGroups = Language == Lang::Go;
	Reading.HasToolchainCommentDirectives = Language == Lang::Go;
	Reading.HasHeaderBuildConstraints = Language.IsCFamily();
	Reading.HasPositionDirectiveComments = Language == Lang::Go || Language.IsCFamily();
	Reading.ReadsUnicodeSpaces = Language.IsJsTs() || Language == Lang::CSharp;
	Reading.ReadsByteOrderMarkAsSpace = Language.IsJsTs();
	if(Language == Lang::C && Language.IsAmbiguous) {
		Reading.ForeignDialectHeads = { "#import", "@implementation", "@interface", "@protocol" };
	}
	if(Language == Lang::TypeScript) Reading.MarkupDocumentHeads = { "<!DOCTYPE", "<?xml" };
	if(Language == Lang::HTML) {
		Reading.RawTextEscapeElement = "script";
		Reading.RawTextEscapeOpen = "<!--";
		Reading.RawTextEndOpen = "</";
		Reading.RawTextNameTerminators = "\t\n\f\r />";
		Reading.RawTextPlainElement = "style";
	}
	// 行コメントの登録形の判定条件（登録定義の内の記法・本文・本文の中の位置の判定に要る物）
	Core::RegisteredLineForms &Forms = Rules.Notation.OutputForms.Conditions;
	if(Language == Lang::Ruby || Language == Lang::Python) {
		Forms.EncodingDeclarationWord = "coding";
		Forms.EncodingDeclarationSeparators = ":=";
	}
	if(Language == Lang::Python) {
		// 登録語の大小文字を区別して直後の区切りと名前を読み，２行目は行頭の空白と書式送りの後の記号だけを読む (PEP 263)
		Forms.EncodingNameChars = "-_.";
		Reading.EncodingLineIndent = " \t\f";
	}
	if(Language == Lang::Ruby) {
		// 注記全体の組か囲みの中の組を先に読み，組として読めない注記だけから大小文字を問わず空白を挟み得る登録語を探す
		Forms.IsEncodingWordCaseless = true;
		Forms.AllowsEncodingSeparatorSpace = true;
		Forms.EncodingMagicCommentNames = { "coding", "encoding" };
		// 文字列の凍結は最初の字句より前，定数の共有はコメントだけの行，字下げの警告は全てのコメントで読む
		Reading.MagicCommentWords = {
			{ "frozen_string_literal", MagicCommentScope::Header, { "false", "true" } },
			{
				"shareable_constant_value",
				MagicCommentScope::CommentLine,
				{ "experimental_copy", "experimental_everything", "literal", "none" }
			},
			{ "warn_indent", MagicCommentScope::Anywhere, { "false", "true" } }
		};
		for(const MagicCommentWord &Word : Reading.MagicCommentWords) Forms.MagicCommentNames.push_back(Word.Name);
		Forms.MagicCommentMarker = "-*-";
		Forms.MagicCommentSeparators = ":";
		Forms.MagicCommentDelimiters = ";";
		Forms.MagicCommentQuotes = "\"'";
		Forms.MagicCommentNameJoiners = "-_";
	}
	if(Language == Lang::Go) {
		Reading.OutputFunctionPrefix = "Example";
		Reading.OutputCommentWords = { "output:", "unordered output:" };
	}
	Reading.HasHaltedData = Language == Lang::PHP;
	Reading.HaltCallName = Language == Lang::PHP ? "__halt_compiler" : "";
	Reading.ReadsLoneCrAsNewline = Language != Lang::Go && Language != Lang::Rust && Language != Lang::Ruby;
	if(Language == Lang::CSharp) Reading.UnicodeLineSeparators = { "\xC2\x85", "\xE2\x80\xA8", "\xE2\x80\xA9" };
	if(Language.IsJsTs()) Reading.UnicodeLineSeparators = { "\xE2\x80\xA8", "\xE2\x80\xA9" };
	Reading.HasOuterText = Language == Lang::PHP;
	Reading.HasEmbeddedScripts = Language == Lang::HTML;
	Reading.HasJsxText = Language == Lang::JavaScript || Language.IsJsx;
	Reading.HasAmbiguousHeaderDialect = Language == Lang::C && Language.IsAmbiguous;
	Reading.MayContainMarkupDocument = Language == Lang::TypeScript;
	Reading.HeaderPromotion = Language == Lang::C && Language.IsAmbiguous ? Lang::Cpp : Lang::Unknown;
	Reading.MasksEscapedBackslashes = Language == Lang::CSS;
	Reading.SelectorCommentActsAsSpace = Language == Lang::CSS && Language.IsScss;
	Reading.MayMisreadTemplateArguments = Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::TypeScript;
	Reading.MayMisreadGenericCalls = Language == Lang::TypeScript;
	Reading.MayMisreadTemplateDeclarations = Language == Lang::Cpp;
	Reading.KeepsImportNamesTogether = Language == Lang::Java || Language == Lang::Swift;
	Reading.MisreadsCommandArgumentBoundary = Language == Lang::Ruby;
	Reading.HasKeywordClosedBlocks = Language == Lang::Ruby;
	Reading.MisreadsCasts = Language == Lang::CSharp;
	Reading.HasIndentBlocks = Language == Lang::Python;
	Reading.HasLineSensitiveBoundary = Language.IsJsTs() || Language == Lang::Go || Language == Lang::Swift;
	Reading.CommentLineSeparatorsEndStatements = Language.IsJsTs();
	const std::span<const std::string_view> Prefixes = StringPrefixes(Language);
	Reading.StringPrefixes.assign(Prefixes.begin(), Prefixes.end());
	if(Language == Lang::Rust) Reading.HashQuotePrefixes = { "br", "cr", "r" };
	if(Language == Lang::CSharp) Reading.VerbatimStringPrefixes = { "$@", "@", "@$" };
	if(Language == Lang::Python) Reading.MultilineQuotes = { "\"\"\"", "'''" };
	if(Language == Lang::CSharp || Language == Lang::Java || Language == Lang::Kotlin || Language == Lang::Swift) {
		Reading.MultilineQuotes = { "\"\"\"" };
	}
	if(Language.IsJsTs()) {
		Reading.CommentLineBreaks = { "\n", "\r" };
		Reading.CommentLineBreaks.insert(
			Reading.CommentLineBreaks.end(),
			Reading.UnicodeLineSeparators.begin(),
			Reading.UnicodeLineSeparators.end()
		);
	}
	if(Language == Lang::Swift) Reading.CommentLineBreaks = { "\n", "\r" };
	Reading.HasRestrictedLineWords = Language.IsJsTs();
	Reading.RestrictedLineArrow = Language.IsJsTs() ? "=>" : "";
	Reading.LeadingBinaryOperatorChars = Language == Lang::Swift ? "/=-+!*%<>&|^~?" : "";
	Reading.ExpressionContinuationChars =
	Language.IsJsTs() || Language == Lang::Go || Language == Lang::Swift ? "([{,;:=?+-*%&|^~<>." : "";
	Reading.HasLeadingBinaryOperators = Language == Lang::Swift;
	Reading.HasShortVariableDeclarations = Language == Lang::Go;
	Reading.HasCompositeHeaderExpression = Language == Lang::Go;
	Reading.HasStructHeaderExpression = Language == Lang::Rust;
	Reading.HasPrefixCallHeaderExpression = Language == Lang::Swift;
	Reading.HasContextualTypeRepresentation = Language == Lang::Swift;
	Reading.HasPreprocessorMacros = Language.IsCFamily();
	Reading.HasCommentAbsorbingDeclarations = Language == Lang::Go;
	Reading.HasUnsupportedMarkupComments = Language.IsJsTs();
	Reading.ScriptAcceptsMarkupComments = Language == Lang::JavaScript;
	Reading.MarkupCommentOpen = Language.IsJsTs() ? "<!--" : "";
	Reading.MarkupCommentClose = Language.IsJsTs() ? "-->" : "";
	Reading.HasArrowChainMisreads = Language.IsJsTs();
	Rules.Notation.LineCommentTerminator = Language == Lang::PHP ? "?>" : "";
	// 行コメントを行の途中で終える改行の字面（改行と読む Unicode の区切りと Sass の改頁と，保護範囲に残る単独の CR），行末で次の行を繋ぐ字面（逆斜線と其の三連字），字句化前の Unicode の逃避
	Rules.Notation.LineCommentBreaks =
	Language == Lang::CSS ? std::vector<std::string_view>{ "\f" } : Reading.UnicodeLineSeparators;
	if(Reading.ReadsLoneCrAsNewline) Rules.Notation.LineCommentBreaks.push_back("\r");
	if(Language.IsCFamily()) Rules.Notation.LineSplices = { "?\?/", "\\" };
	Rules.Notation.DecodesUnicodeEscapes = Language == Lang::Java;
	if(Language == Lang::PHP) Rules.CodeOpeningSuffixes = { "<?=", "<?php" };
	Rules.Notation.BlockCommentInterruptsStatementHead = Language == Lang::Swift;
	if(Language == Lang::TypeScript) {
		Reading.ReferenceDirectiveElements = { "<amd-dependency", "<amd-module", "<reference" };
		Reading.ReferenceDirectiveTerminators = " \t/";
	}
	Rules.Notation.HasBracketedDocumentType = Language == Lang::Ruby;
	Rules.Notation.HasBareDocumentType = Language == Lang::PHP;
	if(Language == Lang::PHP) {
		Rules.Notation.DocumentParameterSigil = "$";
		Rules.Notation.DocumentParameterMarks = { "&", "..." };
	}
	Rules.Notation.ExcludesDirectiveLines = Language == Lang::Go;
	// コメントの中のコード例の書式（godoc は字下げでコード例を示し囲み・要素を持たない，本文は行頭に字下げを持たない為に引用・箇条の印の後の字下げだけが残る，C# の XML 文書は `<c>` と `<code>` の要素）
	Rules.Notation.CodeExamples = {
		Language == Lang::Go,
		Language != Lang::Go,
		{ "{@code", "{@literal" },
		"@\\",
		{ "code", "verbatim" },
		"end",
		'@',
		Language == Lang::Go ?
		std::vector<std::string_view>{} :
		Language == Lang::CSharp ? std::vector<std::string_view>{ "c", "code" } : std::vector<std::string_view>{ "code", "pre" },
		Language == Lang::Go ?
		std::vector<std::pair<std::string_view, std::string_view>>{} :
		std::vector<std::pair<std::string_view, std::string_view>>{ { "<!--", "-->" }, { "<![CDATA[", "]]>" } }
	};
	Rules.Notation.HasOptionalFailureType = Language.IsJsTs();
	Rules.Notation.MemberDocumentHead = Language.IsCFamily() ? "<" : "";
	// 文書に使う記法と其の入力の文書形式（同じ形式の行と囲みの文書だけを揃え，Java の Markdown と従来形式は揃えない，整形仕様 7.6）
	// 囲みの文書が無い時は先頭の記法を出力に採る為，記法の優先順を保つ
	using Form = Core::DocumentFormId;
	if(Language.IsCFamily()) {
		Rules.Notation.DocumentForms = {
			{ CommentNotation::TripleSlashLine, Form::Doxygen },
			{ CommentNotation::SlashBangLine, Form::Doxygen },
			{ CommentNotation::DoubleStarBlock, Form::Doxygen },
			{ CommentNotation::StarBangBlock, Form::Doxygen }
		};
	}
	if(Language == Lang::CSharp) {
		Rules.Notation.DocumentForms =
		{ { CommentNotation::TripleSlashLine, Form::XmlDocumentation }, { CommentNotation::DoubleStarBlock, Form::XmlDocumentation } };
	}
	if(Language == Lang::Java) {
		Rules.Notation.DocumentForms =
		{ { CommentNotation::TripleSlashLine, Form::JavadocMarkdown }, { CommentNotation::DoubleStarBlock, Form::Javadoc } };
	}
	if(Language == Lang::Go) {
		Rules.Notation.DocumentForms = { { CommentNotation::SlashLine, Form::Godoc }, { CommentNotation::Block, Form::Godoc } };
	}
	if(Language == Lang::Rust) {
		Rules.Notation.DocumentForms = {
			{ CommentNotation::TripleSlashLine, Form::Rustdoc },
			{ CommentNotation::SlashBangLine, Form::Rustdoc },
			{ CommentNotation::DoubleStarBlock, Form::Rustdoc },
			{ CommentNotation::StarBangBlock, Form::Rustdoc }
		};
	}
	if(Language == Lang::Kotlin) Rules.Notation.DocumentForms = { { CommentNotation::DoubleStarBlock, Form::KDoc } };
	if(Language == Lang::Swift) {
		Rules.Notation.DocumentForms =
		{ { CommentNotation::TripleSlashLine, Form::SwiftMarkup }, { CommentNotation::DoubleStarBlock, Form::SwiftMarkup } };
	}
	if(Language == Lang::PHP) Rules.Notation.DocumentForms = { { CommentNotation::DoubleStarBlock, Form::PHPDoc } };
	if(Language.IsJsTs()) Rules.Notation.DocumentForms = { { CommentNotation::DoubleStarBlock, Form::JSDoc } };
	if(Language == Lang::Ruby) {
		Rules.Notation.DocumentForms = { { CommentNotation::HashLine, Form::Yard }, { CommentNotation::HashColonLine, Form::Yard } };
	}
	// 通常の独立・行末コメント，文書，区切の用途毎の採用記法（整形仕様 7.6，HTML のコメントは文書の内容）
	if(Language != Lang::HTML) {
		using Use = Core::CommentUse;
		const CommentNotation Plain = Language == Lang::Ruby || Language == Lang::Python ?
		CommentNotation::HashLine :
		Language == Lang::CSS && !Language.IsScss ? CommentNotation::Block : CommentNotation::SlashLine;
		Rules.Notation.OutputForms.Forms.push_back({ Use::Plain, Plain, "" });
		// 文書は囲みの文書の形式を持つ言語で其の囲み，其の他は文書に使う行の記法
		if(Rules.Notation.DocumentFormOf(CommentNotation::DoubleStarBlock)) {
			Rules.Notation.OutputForms.Forms.push_back({ Use::Document, CommentNotation::DoubleStarBlock, "" });
		} else if(!Rules.Notation.DocumentForms.empty()) {
			Rules.Notation.OutputForms.Forms.push_back({ Use::Document, Rules.Notation.DocumentForms.front().first, "" });
		}
		// 区切は二重星の囲み（Go は二重星を記法に持たない為に一般の囲みへ星を密着させる），文書属性と Sass と井桁の言語と CSS は通常コメントの記法
		if(Language == Lang::Go) Rules.Notation.OutputForms.Forms.push_back({ Use::Separator, CommentNotation::Block, "*" });
		else if(Language == Lang::Rust || Language == Lang::Ruby || Language == Lang::Python || Language == Lang::CSS) {
			Rules.Notation.OutputForms.Forms.push_back({ Use::Separator, Plain, "" });
		} else if(Language != Lang::JSON) {
			Rules.Notation.OutputForms.Forms.push_back({ Use::Separator, CommentNotation::DoubleStarBlock, "" });
		}
	}
	// 文書注記の属性と配置
	Rules.Notation.DocumentCommentIsAttribute = Language == Lang::Rust;
	Rules.Notation.DocumentAttributeOpen = Language == Lang::Rust ? "#[" : "";
	Rules.Notation.InlineCommentPrecludesDocument = Language == Lang::Kotlin;
	// 脱出と返戻の読取規則
	Rules.HasLabelJump = Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::Go || Language == Lang::PHP;
	Rules.LabelJumpIsScoped = Language == Lang::CSharp;
	Rules.ExitHasNumericDepth = Language == Lang::PHP;
	Rules.ContinueCountsSwitch = Language == Lang::PHP;
	Rules.ExitLabelIsBodyChild = Language == Lang::Rust;
	Rules.ExitLabelIsPreviousSibling = Language == Lang::Kotlin || Language == Lang::Swift;
	Rules.CaseMayNestInStatement = Language.IsCFamily();
	Rules.CaseOpensWithStatementEnd = Language == Lang::PHP;
	Rules.CaseFallsThrough = Language.IsCFamily() || Language.IsJsTs() || Language == Lang::PHP;
	Rules.ColonCaseFallsThrough = Language == Lang::Java;
	Rules.EmptyCaseFallsThrough = Language == Lang::CSharp;
	Rules.OmittedResult = Language == Lang::C ?
	Core::OmittedResultNotation::Integer :
	Language == Lang::Cpp || Language == Lang::Go || Language == Lang::Rust || Language == Lang::Kotlin || Language == Lang::Swift ?
	Core::OmittedResultNotation::Unit :
	Core::OmittedResultNotation::Inferred;
	Rules.TailExpressionIsValue = Language == Lang::Rust;
	Rules.ReachabilityUsesConstantCondition = Language == Lang::CSharp || Language == Lang::Java;
	// 数値とリテラルの読取規則
	Rules.BasePrefixLetters = Language == Lang::Rust || Language == Lang::Swift ? "xbo" : "";
	Rules.DigitSeparator =
	Language.IsCFamily() ? '\'' : Language == Lang::JSON || Language == Lang::HTML || Language == Lang::CSS ? '\0' : '_';
	// 各欄は１６進・２進・８進の順に添字で参照する
	if(Language == Lang::PHP) Rules.ExitDepthBasePrefixes = { "xX", "bB", "oO" };
	if(Language == Lang::Rust) {
		// 最初に一致した接尾辞を採る為，長い物を先に置く
		Rules.NumericTypeSuffixes =
		{ "usize", "isize", "u128", "i128", "f32", "f64", "u64", "i64", "u32", "i32", "u16", "i16", "u8", "i8" };
	}
	Rules.QuotePrefixes = Language == Lang::Python ? "bfru" : Language == Lang::PHP ? "b" : "";
	Rules.RawQuotePrefixes = Language == Lang::Python ? "r" : "";
	Rules.FormatQuotePrefixes = Language == Lang::Python ? "f" : "";
	Rules.NonDocumentQuotePrefixes = Language == Lang::Python ? "bf" : "";
	Rules.SingleQuoteBackslashIsLiteral = Language == Lang::PHP || Language == Lang::Ruby;
	Rules.InterpolatesDollar = Language == Lang::PHP;
	Rules.HashInterpolationHeads = Language == Lang::Ruby ? "{@$" : "";
	if(Language == Lang::Ruby) Rules.RegexNamedCaptures = { { "(?'", "'" }, { "(?<", ">" } };
	Rules.HasTripleQuote = Language == Lang::Python;
	Rules.BooleanNameNeedsDeclaration = Language == Lang::C || Language.IsCShared;
	Rules.PadDecimalPoint = Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::Java;
	Rules.TrailingCommaHasEmptyValue = Language == Lang::CSS;
	Rules.SingletonCommaDeterminesTuple = Language == Lang::Python;
	Rules.LineContinuesOnlyInBrackets = Language == Lang::Python;
	Rules.SplitBindingsBeforeConst = Language.IsJsTs();
	Rules.NormalizeTypeAssociation = Language == Lang::TypeScript;
	Rules.NormalizeTupleWrapping = Language == Lang::Python;
	Rules.CallFormsMayDeclare = Language.IsCFamily();
	Reading.EmptyStatementDefinition = Find("empty_statement", true);
	Rules.FalseDefinition = Find("false", true);
	Rules.ExpressionStatementDefinition = Find("expression_statement", true);
	Rules.ReturnExpressionDefinition = Find("return_expression", true);
	Rules.ParenthesizedStatementsDefinition = Find("parenthesized_statements", true);
	Rules.TrueDefinition = Find(Language == Lang::PHP ? "boolean" : "true", true);
	Rules.LoopExpressionDefinition = Find("loop_expression", true);
	Rules.ImplicitParameterDefinition = Find("implicit_parameter", true);
	Rules.DoubleQuotedDefinition = Language == Lang::PHP ? Find("encapsed_string", true) : nullptr;
	Rules.PrefixUnaryDefinition = Find("prefix_unary_expression", true);
	Rules.PrefixDefinition = Find("prefix_expression", true);
	// 処理系が定める名前付の指示記法を使用可能な記法へ登録（名前の後の区切りにタブも読むか，記号は記法の定義が持つ）
	if(Language == Lang::Go) {
		Rules.Notation.AvailableNotations.insert(
			Rules.Notation.AvailableNotations.end(),
			{
				{ CommentNotation::CgoExportLine, false },
				{ CommentNotation::GccgoExternLine, false },
				{ CommentNotation::GoBuildLine, true },
				{ CommentNotation::GoDebugLine, true },
				{ CommentNotation::GoEmbedLine, true },
				{ CommentNotation::GoGenerateLine, true },
				{ CommentNotation::GoLinknameLine, false },
				{ CommentNotation::GoNoescapeLine, false },
				{ CommentNotation::GoNoinlineLine, false },
				{ CommentNotation::GoNoraceLine, false },
				{ CommentNotation::GoNosplitLine, false },
				{ CommentNotation::GoUintptrescapesLine, false },
				{ CommentNotation::GoWasmexportLine, false },
				{ CommentNotation::GoWasmimportLine, false },
				{ CommentNotation::GoLineLine, false },
				{ CommentNotation::GoLineBlock, false }
			}
		);
	} else if(Language.IsCFamily()) Rules.Notation.AvailableNotations.push_back({ CommentNotation::GoBuildLine, true });
	if(Language.IsCFamily()) {
		Rules.Notation.DocumentTagSynonyms = {
			{ "@exception", "@throws" },
			{ "@result", "@return" },
			{ "@returns", "@return" },
			{ "@throw", "@throws" },
			{ "\\exception", "@throws" },
			{ "\\param", "@param" },
			{ "\\result", "@return" },
			{ "\\return", "@return" },
			{ "\\returns", "@return" },
			{ "\\throw", "@throws" },
			{ "\\throws", "@throws" }
		};
	}
	if(Language.IsJsTs()) {
		Rules.Notation.DocumentTagSynonyms =
		{ { "@arg", "@param" }, { "@argument", "@param" }, { "@exception", "@throws" }, { "@returns", "@return" } };
	}
	if(Language == Lang::Java || Language == Lang::Kotlin) Rules.Notation.DocumentTagSynonyms = { { "@exception", "@throws" } };
	// 共通工程が照合・生成する語の綴り（整形仕様 3.1, 3.5）
	const auto SetWord = [this](const Core::Word Meaning, std::vector<std::string_view> &&Spellings) -> void {
		Rules.Words[static_cast<size_t>(Meaning)] = std::move(Spellings);
	};
	const bool IsC = Language.IsCFamily(), IsCpp = Language == Lang::Cpp, IsCSharp = Language == Lang::CSharp;
	const bool IsJava = Language == Lang::Java, IsGo = Language == Lang::Go, IsRust = Language == Lang::Rust;
	const bool IsKotlin = Language == Lang::Kotlin, IsSwift = Language == Lang::Swift, IsPhp = Language == Lang::PHP;
	const bool IsJs = Language.IsJsTs(), IsTs = Language == Lang::TypeScript, IsRuby = Language == Lang::Ruby;
	const bool IsPython = Language == Lang::Python;
	const bool IsCode = IsC || IsCSharp || IsJava || IsGo || IsRust || IsKotlin || IsSwift || IsPhp || IsJs || IsRuby || IsPython;
	if(IsCSharp || IsRust || IsSwift || IsJs || IsPython) SetWord(Core::Word::AsyncModifier, { "async" });
	if(Language.IsCFamily()) SetWord(Core::Word::AlternateBranch, { "else" });
	if(IsCSharp) SetWord(Core::Word::AsyncVoidType, { "Task", "ValueTask" });
	if(IsTs) SetWord(Core::Word::AsyncVoidType, { "Promise<void>" });
	if(IsJs) SetWord(Core::Word::AwaitName, { "await" });
	if(IsKotlin) SetWord(Core::Word::ObjectHead, { "object" });
	if(IsKotlin) SetWord(Core::Word::LabeledReturn, { "return@" });
	if(IsPhp) SetWord(Core::Word::BareCall, { "die" });
	if(IsRuby) SetWord(Core::Word::BareCapture, { "&", "*", "**" });
	if(IsJava || IsKotlin || IsSwift || IsJs || IsRuby) SetWord(Core::Word::BaseReference, { "super" });
	if(IsRuby) SetWord(Core::Word::BlockEnd, { "end" });
	if(IsC) SetWord(Core::Word::Boolean, { "_Bool", "bool" });
	if(IsC) SetWord(Core::Word::StringizingAssertion, { "assert" });
	if(IsPhp) SetWord(Core::Word::ValueOutputTag, { "<?=" });
	if(IsPython) SetWord(Core::Word::LineContinuation, { "\\" });
	// 戻らない呼出は修飾の有無を問わず処理系が同じ関数と読む綴りを並べる
	if(IsC) {
		SetWord(
			Core::Word::TerminatingCall,
			{
				"::_Exit",
				"::abort",
				"::exit",
				"::longjmp",
				"::quick_exit",
				"::std::_Exit",
				"::std::abort",
				"::std::exit",
				"::std::longjmp",
				"::std::quick_exit",
				"::std::rethrow_exception",
				"::std::terminate",
				"::std::throw_with_nested",
				"::std::unreachable",
				"::thrd_exit",
				"_Exit",
				"__builtin_trap",
				"__builtin_unreachable",
				"abort",
				"exit",
				"longjmp",
				"quick_exit",
				"rethrow_exception",
				"std::_Exit",
				"std::abort",
				"std::exit",
				"std::longjmp",
				"std::quick_exit",
				"std::rethrow_exception",
				"std::terminate",
				"std::throw_with_nested",
				"std::unreachable",
				"terminate",
				"thrd_exit",
				"throw_with_nested",
				"unreachable"
			}
		);
	}
	if(IsCSharp) {
		SetWord(
			Core::Word::TerminatingCall,
			{ "Environment.Exit", "Environment.FailFast", "System.Environment.Exit", "System.Environment.FailFast" }
		);
	}
	if(IsGo) SetWord(Core::Word::TerminatingCall, { "panic" });
	if(IsPhp) SetWord(Core::Word::TerminatingCall, { "die", "exit" });
	if(IsPython) SetWord(Core::Word::TerminatingCall, { "exit", "os._exit", "os.abort", "quit", "sys.exit" });
	if(IsJava) SetWord(Core::Word::TerminatingCall, { "System.exit", "java.lang.System.exit" });
	if(IsRust) {
		SetWord(Core::Word::TerminatingCall, { "process::abort", "process::exit", "std::process::abort", "std::process::exit" });
		SetWord(
			Core::Word::TerminatingMacro,
			{
				"core::panic",
				"core::todo",
				"core::unimplemented",
				"core::unreachable",
				"panic",
				"std::panic",
				"std::todo",
				"std::unimplemented",
				"std::unreachable",
				"todo",
				"unimplemented",
				"unreachable"
			}
		);
	}
	if(IsSwift) SetWord(Core::Word::TerminatingCall, { "fatalError", "preconditionFailure" });
	if(IsKotlin) SetWord(Core::Word::TerminatingCall, { "TODO", "error" });
	if(IsRuby) SetWord(Core::Word::SideEffectMethod, { "p", "pp", "print", "puts", "raise" });
	if(IsRuby) SetWord(Core::Word::DocumentableCall, { "attr", "attr_accessor", "attr_reader", "attr_writer" });
	// 関数・型の名前の大文字小文字を区別しない文法の語
	if(IsPhp) for(const Core::Word Meaning : { Core::Word::NeverType, Core::Word::TerminatingCall, Core::Word::VoidType }) {
		Rules.CaseInsensitiveWords.set(static_cast<size_t>(Meaning));
	}
	if(IsCode && !IsRust && !IsKotlin) SetWord(Core::Word::Case, { "case" });
	if(IsCSharp || IsRust || IsKotlin || IsSwift || IsTs) SetWord(Core::Word::Cast, { "as" });
	if(IsGo) SetWord(Core::Word::Channel, { "chan" });
	if(IsRuby) SetWord(Core::Word::ConditionThen, { "then" });
	if(IsCpp) SetWord(Core::Word::ConstantCondition, { "constexpr" });
	if(IsJava) SetWord(Core::Word::ConstantDeclaration, { "final" });
	else if(IsC || IsCSharp || IsGo || IsRust || IsKotlin || IsPhp || IsJs) SetWord(Core::Word::ConstantDeclaration, { "const" });
	if(IsC) SetWord(Core::Word::ConstQualifier, { "const" });
	if(IsCpp || IsCSharp || IsJava || IsPhp || IsJs) SetWord(Core::Word::Construct, { "new" });
	if(IsC || IsCSharp || IsJava || IsGo || IsSwift || IsPhp || IsJs) SetWord(Core::Word::Default, { "default" });
	if(IsRuby || IsPython) SetWord(Core::Word::Define, { "def" });
	if(IsKotlin) SetWord(Core::Word::DeferredInitialization, { "lateinit" });
	if(IsPython) SetWord(Core::Word::Delegation, { "from" });
	if(IsGo) SetWord(Core::Word::DiscardName, { "_" });
	if(IsJs) SetWord(Core::Word::DynamicScope, { "eval" });
	if(IsC) SetWord(Core::Word::EntryFunction, { "main" });
	if(IsC) SetWord(Core::Word::EntryResultType, { "int", "signed", "signedint" });
	if(IsKotlin || IsRuby) SetWord(Core::Word::ExpressionBody, { "=" });
	if(IsCSharp || IsPhp || IsJs) SetWord(Core::Word::ExpressionBody, { "=>" });
	if(IsGo || IsSwift) SetWord(Core::Word::Fallthrough, { "fallthrough" });
	if(IsCode) SetWord(Core::Word::False, { IsPython ? "False" : "false" });
	if(IsGo) SetWord(Core::Word::FunctionHead, { "func" });
	if(IsJs) SetWord(Core::Word::FunctionScopedBinding, { "var" });
	if(IsCSharp || IsKotlin || IsSwift || IsJs) SetWord(Core::Word::Getter, { "get" });
	if(IsJs) SetWord(Core::Word::ImmutableBinding, { "const" });
	if(IsCode) SetWord(Core::Word::IntegerTruth, { "1" });
	if(IsCode) SetWord(Core::Word::IntegerZero, { "0" });
	if(IsJava) SetWord(Core::Word::ImmutableBinding, { "final" });
	if(IsKotlin) SetWord(Core::Word::ImmutableBinding, { "val" });
	if(IsCSharp || IsRust || IsKotlin || IsSwift || IsJs || IsRuby || IsPython) SetWord(Core::Word::In, { "in" });
	if(IsCSharp || IsJava) SetWord(Core::Word::InferredType, { "var" });
	if(IsC) {
		SetWord(
			Core::Word::IntegerAlias,
			{
				"int16_t",
				"int32_t",
				"int64_t",
				"int8_t",
				"intmax_t",
				"intptr_t",
				"ptrdiff_t",
				"size_t",
				"uint16_t",
				"uint32_t",
				"uint64_t",
				"uint8_t",
				"uintmax_t",
				"uintptr_t"
			}
		);
	}
	if(IsRuby) SetWord(Core::Word::IterationDo, { "do" });
	if(IsJs) SetWord(Core::Word::IterationOf, { "of" });
	if(IsKotlin) SetWord(Core::Word::LabeledReceiver, { "super@", "this@" });
	if(IsRuby || IsPython) SetWord(Core::Word::LogicalWord, { "and", "or" });
	if(Language == Lang::HTML) SetWord(Core::Word::MetadataElement, { "head" });
	if(Language == Lang::HTML) SetWord(Core::Word::PreserveSpaceAttribute, { "xml:space" });
	if(Language == Lang::HTML) SetWord(Core::Word::InlineStyleAttribute, { "style" });
	if(Language == Lang::HTML) SetWord(Core::Word::NamespaceAttribute, { "xmlns" });
	if(IsCSharp) SetWord(Core::Word::OutputArgument, { "out" });
	if(IsJs) SetWord(Core::Word::ClassHead, { "class" });
	if(IsSwift) SetWord(Core::Word::MetatypeSuffix, { "Type" });
	if(IsPhp) SetWord(Core::Word::LogicalWord, { "and", "or", "xor" });
	if(IsRust) SetWord(Core::Word::Loop, { "loop" });
	if(IsRuby || IsPython) SetWord(Core::Word::Negation, { "not" });
	if(IsKotlin) SetWord(Core::Word::NeverType, { "Nothing" });
	if(IsSwift) SetWord(Core::Word::NeverType, { "Never" });
	if(IsPhp || IsTs) SetWord(Core::Word::NeverType, { "never" });
	if(IsPython) SetWord(Core::Word::NeverType, { "Never", "NoReturn" });
	if(IsRust) SetWord(Core::Word::NeverType, { "!" });
	if(IsC) SetWord(Core::Word::NoReturnSpecifier, { "_Noreturn", "__noreturn__", "noreturn" });
	if(IsSwift) SetWord(Core::Word::OptionalityMarker, { "?" });
	if(IsJs) SetWord(Core::Word::ReassignableBinding, { "let" });
	if(IsKotlin) SetWord(Core::Word::ReassignableBinding, { "var" });
	if(IsRust) SetWord(Core::Word::ReferenceBinding, { "ref" });
	if(IsRuby) SetWord(Core::Word::RegexDelimiter, { "/" });
	if(IsCode) SetWord(Core::Word::Return, { "return" });
	if(IsCpp || IsCSharp || IsJava || IsKotlin || IsJs) SetWord(Core::Word::Self, { "this" });
	if(IsRust || IsSwift || IsRuby || IsPython) SetWord(Core::Word::Self, { "self" });
	if(IsCpp) SetWord(Core::Word::StandardNamespace, { "std::" });
	if(IsJs) SetWord(Core::Word::StatementDeclarationHead, { "class", "function" });
	if(IsCode || Language == Lang::CSS) SetWord(Core::Word::StatementEnd, { ";" });
	if(Language == Lang::JSON) SetWord(Core::Word::ListSeparator, { "," });
	if(IsCode) SetWord(Core::Word::True, { IsPython ? "True" : "true" });
	// 生成する真値の綴りは先頭の小文字形に固定する
	if(IsPhp) {
		SetWord(
			Core::Word::True,
			{
				"true",
				"TRUE",
				"TRUe",
				"TRuE",
				"TRue",
				"TrUE",
				"TrUe",
				"TruE",
				"True",
				"tRUE",
				"tRUe",
				"tRuE",
				"tRue",
				"trUE",
				"trUe",
				"truE"
			}
		);
	}
	if(IsGo || IsRust || IsTs || IsPython) SetWord(Core::Word::TypeDeclaration, { "type" });
	if(IsJs) SetWord(Core::Word::TypeQuery, { "typeof" });
	if(IsJava || IsPhp || IsJs) SetWord(Core::Word::TypeTest, { "instanceof" });
	if(IsC || IsPython) SetWord(Core::Word::VariadicMarker, { "..." });
	if(IsC || IsCSharp || IsJava || IsPhp || IsTs) SetWord(Core::Word::VoidType, { "void" });
	if(IsKotlin) SetWord(Core::Word::VoidType, { "Unit" });
	if(IsSwift) SetWord(Core::Word::VoidType, { "()", "Void" });
	if(IsPython) SetWord(Core::Word::VoidType, { "None" });
	if(IsRust) SetWord(Core::Word::VoidType, { "()" });
	if(IsCode && !IsGo) SetWord(Core::Word::While, { "while" });
	if(IsCpp || IsCSharp || IsJava || IsPhp) {
		SetWord(Core::Word::PrivateAccess, { "private" });
		SetWord(Core::Word::ProtectedAccess, { "protected" });
		SetWord(Core::Word::PublicAccess, { "public" });
	}
	if(IsTs) SetWord(Core::Word::DynamicTopType, { "any" });
	if(IsCSharp) SetWord(Core::Word::CatchFilter, { "when" });
	if(IsC || IsCSharp || IsJava || IsKotlin || IsSwift || IsPhp || IsJs) SetWord(Core::Word::CatchHead, { "catch" });
	if(IsCpp) SetWord(Core::Word::CatchQualifier, { "const" });
	if(IsJava) SetWord(Core::Word::CatchQualifier, { "final" });
	if(IsSwift) SetWord(Core::Word::CatchBinding, { "let", "var" });
	if(IsCSharp) SetWord(Core::Word::UniversalException, { "Exception" });
	if(IsJava || IsKotlin || IsPhp) SetWord(Core::Word::UniversalException, { "Exception", "Throwable" });
	if(IsRuby) SetWord(Core::Word::UniversalException, { "Exception", "StandardError" });
	if(IsPython) SetWord(Core::Word::UniversalException, { "BaseException", "Exception" });
	if(IsCpp || IsCSharp || IsJava || IsPhp || IsJs || IsRuby) SetWord(Core::Word::ConstructionMethod, { "new" });
	if(IsCSharp || IsJava || IsGo) SetWord(Core::Word::SignedByteType, { "byte" });
	if(IsC || IsCSharp || IsJava) SetWord(Core::Word::LongIntegerType, { "long" });
	if(IsJava || IsSwift) SetWord(Core::Word::ThrowsMarker, { "throws" });
	if(IsKotlin) SetWord(Core::Word::ThrowsAnnotation, { "Throws(" });
	if(IsKotlin) SetWord(Core::Word::ThrowHead, { "throw" });
	if(IsRuby) SetWord(Core::Word::ImplicitRaise, { "fail", "raise" });
	if(IsC) SetWord(Core::Word::UsingNamespace, { "using namespace" });
	if(IsJs) SetWord(Core::Word::NullishValue, { "null", "undefined" });
	if(IsC || IsCSharp || IsJava || IsKotlin || IsPhp || IsJs) SetWord(Core::Word::StringType, { "String", "string" });
	if(IsRust) {
		SetWord(Core::Word::ErrorResultType, { "Result" });
		SetWord(Core::Word::PanicMacro, { "assert", "assert_eq", "assert_ne", "panic" });
	}
	if(IsCpp) SetWord(Core::Word::NullMacro, { "NULL" });
	if(IsC) SetWord(Core::Word::ConstexprQualifier, { "constexpr" });
	if(IsRust) SetWord(Core::Word::DocumentAttributeName, { "doc" });
	if(IsC) SetWord(Core::Word::FloatingType, { "float" });
	// 中間表現の構築と配置
	Rules.CanContinueWithBrace = Language == Lang::Kotlin || Language == Lang::Swift;
	Rules.CanContinueWithCall = Language == Lang::Kotlin;
	Reading.MisreadsCallContinuation = Language == Lang::Kotlin;
	Rules.CanOmitTrailingSemicolon = Language == Lang::Go || Language == Lang::Kotlin || Language == Lang::Swift;
	Rules.IsWhitespaceSemantic = Language == Lang::CSS;
	Reading.MisreadsSelectorCommentBoundary = Language == Lang::CSS;
	Rules.LexicalSeparator = Language == Lang::CSS ? "/**/" : "";
	Rules.CanBreakAfterAssignment = Language != Lang::Python;
	Rules.HasDeferredLiteralBodies = std::ranges::any_of(
		Kinds,
		[](const Core::KindDefinition &Definition) -> bool {
			return Definition.Roles.test(static_cast<size_t>(Core::Role::DeferredLiteralStart));
		}
	);
	Rules.HasLineDirectives = Language.IsCFamily();
	Rules.LineEdgeSpaceOpen = Language.IsJsTs() ? "{\"" : "";
	Rules.LineEdgeSpaceClose = Language.IsJsTs() ? "\"}" : "";
	Rules.LineEdgeTab = Language.IsJsTs() ? "&#9;" : "";
	Rules.HasAutomaticLineTerminator = Language == Lang::Go;
	Rules.CanAttachTopLevelSemicolon = Language == Lang::Rust;
	// 解析用の本文を編集して読み直す解析器の読違
	Reading.MisbindsConstructionCallee = Language == Lang::TypeScript;
	Reading.MisreadsSoftKeywordCalls = Language == Lang::Python;
	Reading.MisbindsEmbeddedBranches = Language == Lang::PHP;
	Reading.MisbindsPostfixOperand = Language == Lang::Swift;
	Reading.MisassociatesOperators = Language == Lang::Swift;
	// 部分整形を始めれない誤りを持つ入力の条件（整形仕様 4.6，誤りの回復が区画の境界を失わせる文法と既知の読違）
	Reading.RecoveryRestructuresDocument = Language == Lang::HTML;
	Reading.AbsorbsVerbatimOnError = Language == Lang::PHP;
	Reading.VerbatimStart = Language == Lang::PHP ? "<<<" : "";
	Reading.MisreadsCustomProperties = Language == Lang::CSS;
	Reading.AbsorbsRulesIntoValueComments = Language == Lang::CSS;
	Reading.MisreadsEmptyArguments = Language.IsScss;
	Reading.UrlFunctionHead = Language == Lang::CSS ? "url(" : "";
	Reading.BooleanNameHeader = Language.IsCFamily() ? "<stdbool.h>" : "";
	Reading.ImportedForeignPackage = Language == Lang::Go ? "\"C\"" : "";
	Reading.CustomPropertyHead = Language == Lang::CSS ? "--" : "";
	if(Language == Lang::CSS) Reading.ModuleClauseWords = { "as", "hide", "show" };
	Reading.PropertySeparator = Language == Lang::CSS ? ":" : "";
	Reading.MisreadsEscapedRegexEnd = Language == Lang::Swift;
	Reading.DropsTokensOnRecovery = Language == Lang::Kotlin;
	Reading.ToleratesTrailingCommaError = Language == Lang::JSON;
	Reading.AcceptsArrowOperand = Language.IsJsTs();
	Reading.AcceptsContinuationComment = Language == Lang::Python;
	Rules.HasHeadMemberArrow = Language.IsCFamily() || Language == Lang::PHP;
	Rules.HasNumericUnitAdjacency = Language == Lang::CSS;
	Rules.HasSpacedEmptyValue = Language == Lang::CSS;
	Rules.HasBareControlHeader =
	Language == Lang::Go || Language == Lang::Rust || Language == Lang::Swift || Language == Lang::Ruby || Language == Lang::Python;
	if(Language == Lang::Python) Rules.TailOperatorWords = { "//", "@", "and", "else", "if", "in", "is", "is not", "not in", "or" };
	if(Language == Lang::Ruby) Rules.HeadBoundaryWords = { "else", "elsif", "end", "ensure", "rescue", "when" };
	if(Language == Lang::Ruby) Reading.ClauseWords = { "do", "else", "elsif", "end", "ensure", "in", "rescue", "then", "when" };
	if(Language.IsJsTs()) {
		Reading.SeparatingModifiers = { "abstract", "accessor" };
		Reading.JoiningModifiers = { "get", "set", "static" };
	}
	if(Language == Lang::Kotlin) {
		Reading.LineContinuingTokens = { "&&", ".", "?.", "?:", "as", "as?", "by", "catch", "else", "finally", "where", "{", "||" };
	}
	if(Language == Lang::Go) Rules.HeadBoundaryWords = { "case", "default" };
	if(Language.IsCFamily()) {
		Reading.ConditionEndDirective = "endif";
		Reading.DefinitionDirective = "define";
		Reading.UndefinitionDirective = "#undef";
		Reading.LinkageHead = "extern";
		Reading.LinkageOpen = "extern \"C\" {";
		Reading.LinkageClose = "}";
		Reading.CompactLinkageOpen = "extern\"C\"{";
		Reading.LinkageMarkerDirective = "#pragma ";
		Reading.RawStringPrefixes = { "LR", "R", "UR", "u8R", "uR" };
		Reading.VariadicParameter = "__VA_ARGS__";
		Reading.StatementWords = { "break", "continue", "do", "for", "goto", "if", "return", "switch", "while" };
		Reading.ConditionDirectives = { "if", "ifdef", "ifndef" };
		Reading.HeaderNameDirectives = { "import", "include", "include_next" };
		Reading.ParsedDirectives =
		{ "elif", "elifdef", "elifndef", "else", "endif", "if", "ifdef", "ifndef", "import", "include", "include_next", "undef" };
	}
	/** ========== 括弧・波括弧・宣言の正規化 ========== */
	Rules.OperatorPrecedence = {
		{ "!=", 9 },
		{ "!==", 9 },
		{ "!in", 10 },
		{ "!is", 10 },
		{ "!~", 9 },
		{ "%", 13 },
		{ "&", 8 },
		{ "&&", 5 },
		{ "&^", 13 },
		{ "*", 13 },
		{ "**", 14 },
		{ "+", 12 },
		{ "-", 12 },
		{ "..", 10 },
		{ "...", 10 },
		{ "..<", 10 },
		{ "..=", 10 },
		{ "/", 13 },
		{ "//", 13 },
		{ ":", 2 },
		{ "<", 10 },
		{ "<<", 11 },
		{ "<=", 10 },
		{ "<=>", 9 },
		{ "==", 9 },
		{ "===", 9 },
		{ "=~", 9 },
		{ ">", 10 },
		{ ">=", 10 },
		{ ">>", 11 },
		{ ">>>", 11 },
		{ "?", 2 },
		{ "?:", 3 },
		{ "??", 3 },
		{ "@", 13 },
		{ "^", 7 },
		{ "and", 5 },
		{ "as", 10 },
		{ "as?", 10 },
		{ "in", 10 },
		{ "instanceof", 10 },
		{ "is", 10 },
		{ "is not", 10 },
		{ "not in", 10 },
		{ "not_eq", 9 },
		{ "or", 4 },
		{ "rescue", 3 },
		{ "satisfies", 10 },
		{ "|", 6 },
		{ "||", 4 }
	};
	Rules.ComparisonOperators = { "!=", "!==", "!~", "<", "<=", "<=>", "==", "===", "=~", ">", ">=" };
	for(std::pair<const std::string_view, int> &Entry : Rules.OperatorPrecedence) {
		const std::string_view Operator = Entry.first;
		int &Prec = Entry.second;
		const auto Correct = [&]() -> int {
			switch(Language.Id) {
			case Lang::Kotlin:
				// Kotlin の範囲・中置関数・エルビス・型判定・比較の順の返戻
				if(Operator == "as" || Operator == "as?") return 14;
				if(Operator == ".." || Operator == "..<") return 11;
				if(Operator == "?:") return 9;
				if(Operator == "in" || Operator == "!in" || Operator == "is" || Operator == "!is") return 8;
				if(Operator == "<" || Operator == ">" || Operator == "<=" || Operator == ">=") return 7;
				if(Operator == "==" || Operator == "!=" || Operator == "===" || Operator == "!==") return 6;
				break;
			case Lang::Go:
				// Go の `<<` / `>>` / `&` / `&^` の乗除格，`|` / `^` の加減格，比較の同格の左結合の返戻
				if(Operator == "<<" || Operator == ">>" || Operator == "&") return 13;
				if(Operator == "|" || Operator == "^") return 12;
				if(Rules.ComparisonOperators.contains(Operator)) return 10;
				break;
			case Lang::Swift:
				// Swift の << >> の乗除より上で前置単項より下，& の乗除格，| ^ の加減格，比較の加減より下の返戻
				if(Operator == "<<" || Operator == ">>") return 14;
				if(Operator == "&") return 13;
				if(Operator == "|" || Operator == "^") return 12;
				if(Rules.ComparisonOperators.contains(Operator)) return 7;
				if(Operator == "??") return 8;
				break;
			case Lang::Rust:
			case Lang::Python:
				// 比較より上のビット演算と，Rust の前置・乗除間の as，論理・代入間の範囲の返戻
				if(Operator == "as" && Language == Lang::Rust) return 14;
				if((Operator == ".." || Operator == "..=") && Language == Lang::Rust) return 3;
				if(Operator == "&") return 10;
				if(Operator == "^") return 9;
				if(Operator == "|") return 8;
				if(
					Rules.ComparisonOperators.contains(Operator) || Operator == "is" || Operator == "in" || Operator == "is not" ||
					Operator == "not in"
				) return 7;
				break;
			case Lang::PHP:
				// PHP の or < xor < and < 代入は数値空間に３段を収めれない為，優先度不明の０として内外の括弧を保護する事の返戻
				if(Operator == "and" || Operator == "or" || Operator == "xor") return 0;
				break;
			case Lang::Ruby:
				// Ruby の低位演算子の再配置（`&` / `|` / `^` は同格とし，混在時の括弧を保護）の返戻
				if(Operator == "&" || Operator == "|" || Operator == "^") return 10;
				if(Operator == "<" || Operator == "<=" || Operator == ">" || Operator == ">=") return 9;
				if(Rules.ComparisonOperators.contains(Operator)) return 8;
				if(Operator == "&&") return 7;
				if(Operator == "||") return 6;
				if(Operator == ".." || Operator == "...") return 5;
				if(Operator == "?" || Operator == ":") return 4;
				if(Operator == "and" || Operator == "or") return 1;
				break;
			}
			// 共通の優先順位の返戻
			return Prec;
		};
		Prec = Correct();
	}
	if(Language == Lang::Swift) {
		Rules.OperatorPrecedence.emplace("&<<", 14);
		Rules.OperatorPrecedence.emplace("&>>", 14);
		Rules.OperatorPrecedence.emplace("&*", 13);
		Rules.OperatorPrecedence.emplace("&+", 12);
		Rules.OperatorPrecedence.emplace("&-", 12);
		Rules.OperatorPrecedence["..."] = 10;
		Rules.OperatorPrecedence["..<"] = 10;
		Rules.OperatorPrecedence["=="] = 7;
		Rules.OperatorPrecedence["!="] = 7;
		Rules.OperatorPrecedence["==="] = 7;
		Rules.OperatorPrecedence["!=="] = 7;
		Rules.OperatorPrecedence["~="] = 7;
		Reading.ReassociatedOperators = {
			"!=",
			"!==",
			"%",
			"&",
			"&&",
			"&*",
			"&+",
			"&-",
			"&<<",
			"&>>",
			"*",
			"+",
			"-",
			"...",
			"..<",
			"/",
			"<",
			"<<",
			"<=",
			"==",
			"===",
			">",
			">=",
			">>",
			"??",
			"^",
			"|",
			"||",
			"~="
		};
		Reading.OperatorStandIns = {
			{ "!=", "==" },
			{ "!==", "!=" },
			{ "%", "/" },
			{ "&", "^" },
			{ "*", "/" },
			{ "+", "-" },
			{ "-", "+" },
			{ "/", "*" },
			{ "<", ">" },
			{ "<<", "^" },
			{ "<=", ">" },
			{ "==", "!=" },
			{ "===", "==" },
			{ ">", "<" },
			{ ">=", "<" },
			{ ">>", "^" },
			{ "^", "&" },
			{ "|", "^" }
		};
	}
	Rules.AssignmentOperators =
	{ "%=", "&&=", "&=", "&^=", "**=", "*=", "+=", "-=", "//=", "/=", "<<=", "=", ">>=", ">>>=", "?\?=", "^=", "|=", "||=" };
	for(const std::string_view Operator : Rules.AssignmentOperators) {
		Rules.OperatorPrecedence.emplace(Operator, Language == Lang::Ruby ? 2 : 1);
	}
	if(Language != Lang::Rust && Language != Lang::Ruby && Language != Lang::Python) Rules.FusedPrefixPairs = { "&&", "++", "--" };
	if(Language == Lang::Ruby) Rules.FusedPrefixPairs.insert("!~");
	if(Language == Lang::Kotlin) Rules.FusedPrefixPairs.insert("!!");
	Rules.FusedRangePairs = { ".." };
	Rules.OperatorFusionChars = "!%&*+-/:<=>?^|~";
	Rules.AssignmentFusionBefore = ".:<>!=+-*/%&|^?~";
	Rules.AssignmentFusionAfter = "=>~";
	Rules.AttachedOperatorPrefixChars = "?!";
	Rules.MisparsedComparisonLeadChars = "=>";
	Rules.OpenRangeClosers = Language == Lang::Ruby ? ")]}," : ")]},;";
	Rules.RequiredValueParenPrecedences = Language == Lang::Ruby ? std::unordered_set<int>{ 1 } : std::unordered_set<int>{};
	Rules.SignsOverPower = Language == Lang::Python ? "-+~" : Language == Lang::Ruby ? "-" : "";
	Rules.PowerPrecedence =
	Language == Lang::Python || Language.IsJsTs() || Language == Lang::PHP || Language == Lang::Ruby ? 14 : -1;
	Rules.AssignmentPrecedence = Language == Lang::Ruby ? 2 : 1;
	Rules.ConditionalPrecedence = Language == Lang::Ruby ? 4 : 2;
	Rules.TypeContinuationTokens = Language.IsJsTs() ?
	std::unordered_set<std::string_view>{ "&", "?", "|" } :
	Language == Lang::Swift ? std::unordered_set<std::string_view>{ "&" } : std::unordered_set<std::string_view>{};
	Rules.NonAssociativePrecedences = Language == Lang::Rust || Language == Lang::Python ?
	std::unordered_set<int>{ 7 } :
	Language == Lang::Swift ? std::unordered_set<int>{ 7, 10, 14 } : Language == Lang::Ruby ?
	std::unordered_set<int>{ 8 } :
	Language == Lang::PHP ? std::unordered_set<int>{ 9, 10 } : std::unordered_set<int>{};
	Rules.RightAssociativePrecedences = Language == Lang::Ruby ? std::unordered_set<int>{ 2, 4 } : Language == Lang::Swift ?
	std::unordered_set<int>{ 1, 2, 8 } :
	Language == Lang::CSharp ? std::unordered_set<int>{ 1, 2, 3 } : std::unordered_set<int>{ 1, 2 };
	if(Language == Lang::Ruby) Rules.ProtectedPrecedencePairs.insert((10 << 5) + 10);
	if(Language.IsJsTs() || Language == Lang::Cpp) {
		Rules.ProtectedPrecedencePairs.insert((9 << 5) + 11);
		Rules.ProtectedPrecedencePairs.insert((10 << 5) + 11);
	}
	if(Language.IsJsTs()) for(const int Prec : { 4, 5 }) {
		Rules.ProtectedPrecedencePairs.insert((3 << 5) + Prec);
		Rules.ProtectedPrecedencePairs.insert((Prec << 5) + 3);
	}
	// 宣言と式の節点種別の参照
	Rules.PatternListDefinition = Find("pattern_list", true);
	Rules.TupleExpressionDefinition = Find("tuple_expression", true);
	Rules.ExpressionListDefinition = Find("expression_list", true);
	Rules.PrefixExpressionDefinition = Find("prefix_expression", true);
	Rules.ExpressionParenDefinition = Find("parenthesized_expression", true);
	Rules.FreezesLexicalBindings = Language.IsJsTs();
	Rules.CanMutateBase = Language == Lang::Rust;
	Rules.ShouldRemoveMutableMarker = Language == Lang::Rust;
	Reading.IdentifierRawPrefix = Language == Lang::Rust ? "r#" : Language == Lang::CSharp ? "@" : "";
	Reading.IdentifierQuote = Language == Lang::Kotlin || Language == Lang::Swift ? "`" : "";
	Reading.LabelDefinitionSuffix = Language == Lang::Kotlin ? "@" : Language == Lang::Swift ? ":" : "";
	Reading.DecodesIdentifierEscapes =
	Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::Java || Language.IsJsTs();
	Reading.RestrictsBasicIdentifierEscapes = Language.IsCFamily();
	Reading.IgnoresIdentifierFormatCharacters = Language == Lang::CSharp || Language == Lang::Java;
	Rules.ShouldPreserveConstantStatus = Language == Lang::Java;
	Rules.ModifierListDefinition = Find("modifiers", true);
	AttachForms();
	// 同じ役割・空行比較の値・名前付の性質を持つ記号の種別を共有する
	KindsBySymbol.reserve(Kinds.size());
	for(const Core::KindDefinition &Kind : Kinds) {
		const std::deque<Core::KindDefinition>::const_iterator Found = std::find_if(
			SharedKinds.begin(),
			SharedKinds.end(),
			[&Kind](const Core::KindDefinition &Existing) -> bool {
				// 共有する構文上の性質の一致の返戻
				return Existing.BlankKey == Kind.BlankKey && Existing.Roles == Kind.Roles && Existing.IsNamed == Kind.IsNamed;
			}
		);
		if(Found == SharedKinds.end()) {
			SharedKinds.push_back(Kind);
			KindsBySymbol.push_back(&SharedKinds.back());
		} else KindsBySymbol.push_back(&*Found);
	}
	const auto Shared = [this](const Core::KindDefinition *const Definition) -> const Core::KindDefinition * {
		// 文法の記号表から引いた定義の共有先の返戻
		return Definition ? KindsBySymbol[Definition - Kinds.data()] : nullptr;
	};
	for(
		const Core::KindDefinition **Slot :
		{
			&Reading.EmptyStatementDefinition,
			&Rules.DoubleQuotedDefinition,
			&Rules.ExpressionListDefinition,
			&Rules.ExpressionParenDefinition,
			&Rules.ExpressionStatementDefinition,
			&Rules.FalseDefinition,
			&Rules.ImplicitParameterDefinition,
			&Rules.LoopExpressionDefinition,
			&Rules.ModifierListDefinition,
			&Rules.ParenthesizedStatementsDefinition,
			&Rules.PatternListDefinition,
			&Rules.PrefixDefinition,
			&Rules.PrefixExpressionDefinition,
			&Rules.PrefixUnaryDefinition,
			&Rules.ReturnExpressionDefinition,
			&Rules.TrueDefinition,
			&Rules.TupleExpressionDefinition
		}
	) *Slot = Shared(*Slot);
	for(auto &[Lexeme, Definition] : Rules.TokenDefinitions) Definition = Shared(Definition);
	// 文法が子に出さない語を持つ節点と其の語の種別（C++ の純粋仮想の `= 0` の `0`，Swift の型付の送出の `throws`）
	static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> HiddenLexemes {
		{ { "pure_virtual_clause", "number_literal" }, { "throws_clause", "throws" } }
	};
	for(const auto &[Host, Lexeme] : HiddenLexemes) {
		if(const Core::KindDefinition *const Kind = Find(Lexeme, true); Kind && Find(Host, true)) {
			Reading.HiddenLexemeKinds.emplace(Host, Kind);
		}
	}
	for(std::vector<Core::CanonicalForm> &Forms : FormDefinitions) {
		for(Core::CanonicalForm &Form : Forms) for(Core::FormPart &Part : Form.Parts) Part.Definition = Shared(Part.Definition);
	}
	for(Core::CanonicalForm &Form : Rules.GeneratedForms) {
		for(Core::FormPart &Part : Form.Parts) Part.Definition = Shared(Part.Definition);
	}
	// 導出の途中にだけ使った記号毎の値を捨て，共有定義だけを残す
	std::vector<Core::KindDefinition>().swap(Kinds);
	// 終了
	return;
}

/**
 * 言語の文法の定義の取得関数
 * 言語と方言の組毎に一度だけ作り，呼出間で変更不能な値として共有する（整形仕様 9.4）
 * @param Language 対象言語と方言
 * @return 文法の定義
 * @throws std::invalid_argument 文法を持たない言語の場合
 */
const GrammarProfile &GrammarProfile::Resolve(const Lang Language) {
	// 文法変種の共有領域と索引の準備
	constexpr size_t VariantCount = 48, Size = static_cast<size_t>(Lang::Unknown) * VariantCount;
	static std::array<std::once_flag, Size> Flags;
	static std::array<std::unique_ptr<GrammarProfile>, Size> Profiles;
	if(Language.Id == Lang::Unknown) throw std::invalid_argument("the language has no grammar");
	const size_t Slot =
	static_cast<size_t>(Language.Id) * VariantCount + Language.IsJsx + (Language.IsScss << 1) + (Language.IsCShared << 2) +
	(Language.IsAmbiguous << 3) + (static_cast<size_t>(Language.Goal) << 4);
	std::call_once(
		Flags[Slot],
		[&]() -> void {
			// 共有する定義は文法の変種（中身で判別する共有拡張子を含む）だけで決め，言語を推定したかの指定の状態を持たない
			Lang Grammar = Language;
			Grammar.IsAssumed = false;
			Profiles[Slot].reset(new GrammarProfile(Grammar));
		}
	);
	// 共有する文法の定義の返戻
	return *Profiles[Slot];
}

/**
 * 対象言語の取得関数
 * @return 対象言語と方言
 */
Lang GrammarProfile::GetLanguage() const {
	// 対象言語の返戻
	return Language;
}

/**
 * 役割を持つ共通の種別の検索関数
 * @param Wanted 探す役割
 * @return 種別（文法に無ければ無し）
 */
const Core::KindDefinition *GrammarProfile::KindWithRole(const Core::Role Wanted) const {
	const Core::KindDefinition *Result = nullptr;
	for(TSSymbol Symbol = 0; Symbol < KindsBySymbol.size(); ++Symbol) {
		if(KindsBySymbol[Symbol]->Roles.test(static_cast<size_t>(Wanted))) Result = KindsBySymbol[Symbol];
	}
	// 最後に対応した種別の返戻
	return Result;
}

/**
 * 役割を持つ解析器の記号の検索関数
 * @param Wanted 探す役割
 * @return 最初に対応した記号（文法に無ければ０）
 */
TSSymbol GrammarProfile::SymbolWithRole(const Core::Role Wanted) const {
	for(TSSymbol Symbol = 0; Symbol < KindsBySymbol.size(); ++Symbol) {
		// 最初に対応する記号の返戻
		if(KindsBySymbol[Symbol]->Roles.test(static_cast<size_t>(Wanted))) return Symbol;
	}
	// 対応する記号が無い場合の返戻
	return 0;
}

/**
 * 記号の節点名の取得関数
 * @param Symbol 文法の記号
 * @return 節点名（記号表の外なら空）
 */
std::string_view GrammarProfile::SymbolName(const TSSymbol Symbol) const {
	// 記号表の内側の名前の返戻
	return Symbol < SymbolNames.size() ? SymbolNames[Symbol] : std::string_view();
}

/**
 * 記号の節点名の一致の判定関数（整形仕様 4.3，節点名の照合を此の定義へ集める）
 * @param Left 文法の記号
 * @param Right 文法の記号
 * @return 同じ節点名の記号なら true（記号表の外の記号は同じ記号だけを一致とする）
 */
bool GrammarProfile::IsSameName(const TSSymbol Left, const TSSymbol Right) const {
	// 記号表の外の記号の一致の返戻
	if(Left >= NameSymbols.size() || Right >= NameSymbols.size()) return Left == Right;
	// 同じ節点名の最初の記号の一致の返戻
	return NameSymbols[Left] == NameSymbols[Right];
}

/**
 * 字句の字面の解析器の記号の取得関数（整形仕様 4.3，節点名の照合を此の定義へ集める）
 * @param Text 字句の字面
 * @return 同じ字面の見える字句の記号（文法の字句でなければ 0）
 */
TSSymbol GrammarProfile::TokenSymbol(const std::string_view Text) const {
	const std::unordered_map<std::string_view, TSSymbol>::const_iterator Found = TokenSymbols.find(Text);
	// 字句の記号の返戻
	return Found == TokenSymbols.end() ? 0 : Found->second;
}

/**
 * 子の役割に対応する文法の場の取得関数
 * @param Role 子の役割
 * @return 其の役割へ写る文法の場の識別子（無ければ空）
 */
std::span<const TSFieldId> GrammarProfile::FieldsOf(const Core::ChildRole Role) const {
	const size_t RoleIndex = static_cast<size_t>(Role);
	// 役割へ写る場の返戻
	return RoleIndex < FieldsByRole.size() ? std::span<const TSFieldId>(FieldsByRole[RoleIndex]) : std::span<const TSFieldId>();
}

/**
 * 文法に在る子の役割の取得関数
 * @return 子の役割の集合
 */
uint64_t GrammarProfile::GetChildRoleMask() const {
	return ChildRoleMask;
}

/**
 * 文法の構文・意味の固定の定義の取得関数
 * @return 言語名を持たない構文・意味の固定の定義（コメントの記法の定義を含む）
 */
const Core::SyntaxRules &GrammarProfile::GetRules() const {
	// 固定の定義の返戻
	return Rules;
}

/**
 * 入力の読取規則の取得関数
 * @return 言語名を持たない入力の読取規則
 */
const ReadingRules &GrammarProfile::GetReading() const {
	// 入力の読取規則の返戻
	return Reading;
}

/**
 * 記号の生成形式の取得関数
 * @param Symbol 文法の記号
 * @return 記号に使える生成形式
 */
Core::CanonicalFormList GrammarProfile::FormsOf(const TSSymbol Symbol) const {
	// 記号表の外には生成形式が無い事の返戻
	return Symbol < FormDefinitions.size() ? Core::CanonicalFormList(FormDefinitions[Symbol]) : Core::CanonicalFormList();
}

/**
 * 記号の共通の節点種別の取得関数
 * @param Symbol 文法の記号
 * @return 種別
 */
const Core::KindDefinition &GrammarProfile::KindOf(const TSSymbol Symbol) const {
	// 記号表の外の記号（解析器の誤り区画）の種別の返戻
	if(Symbol >= KindsBySymbol.size()) return ErrorKind;
	// 種別の返戻
	return *KindsBySymbol.at(Symbol);
}

/**
 * 解析器の節点の記号が役割を持つかの判定関数（整形仕様 4.3，節点名の照合を此の定義へ集める）
 * @param Node 解析器の節点（空なら偽）
 * @param Wanted 役割
 * @return 節点の記号の共通の節点種別が役割を持てば true
 */
bool GrammarProfile::Is(const TSNode Node, const Core::Role Wanted) const {
	// 記号の役割の返戻
	return !ts_node_is_null(Node) && KindOf(ts_node_symbol(Node)).Roles.test(static_cast<size_t>(Wanted));
}

/**
 * 本体の外の地の文の種別の取得関数（整形仕様 4.4）
 * @return 投影が本体の外の地の文を逐語の子として保つ種別
 */
const Core::KindDefinition &GrammarProfile::GetOuterTextKind() const {
	// 種別の返戻
	return OuterTextKind;
}

/**
 * 実行停止後の逐語データの種別の取得関数
 * @return 根の最後に置くデータの種別
 */
const Core::KindDefinition &GrammarProfile::GetTrailingDataKind() const {
	// データの種別の返戻
	return TrailingDataKind;
}

/**
 * 位置を実行時の値として観測する構文の取得関数（整形仕様 4.4）
 * 構文・対象処理系から観測の使用を確かめる為の定義で，行番号らしい文字列は含めない（未知の動的内省は含まない）
 * @return 対象言語の観測する構文の定義（無ければ空）
 */
std::span<const SourceObservation> GrammarProfile::GetObservations() const {
	// 文法の構築時に対応付けた観測する構文の返戻
	return Observations;
}

/**
 * 文書要素の属性が本文の空白を保持するかの判定関数
 * @param AttributeName 属性名
 * @param Value 引用符を除いた属性値
 * @return 空白保持の指定又は未確定の指定なら true
 */
bool GrammarProfile::PreservesMarkupWhitespace(const std::string_view AttributeName, const std::string_view Value) const {
	// 大小文字を問わない属性名
	std::string Name(AttributeName);
	ToLowerAscii(Name);
	const bool IsPreserveSpace = Rules.IsSpelling(Name, Core::Word::PreserveSpaceAttribute);
	// 空白保持を指定しない属性の返戻
	if(!IsPreserveSpace && !Rules.IsSpelling(Name, Core::Word::InlineStyleAttribute)) return false;
	// 文字参照を含む値（復号前の字面で確定出来ない指定）の返戻
	if(Value.find('&') != std::string_view::npos) return true;
	// 空白保持の属性の値又は表示規則の空白保持の指定の返戻
	return IsPreserveSpace ? Value == Reading.PreserveSpaceValue : PreservesStyleWhitespace(Value);
}

/**
 * 要素名の表示と内容の性質の取得関数
 * @param Name 大小文字を揃えた要素名
 * @return 表示・逐語内容・閉じ方の性質の集合
 */
uint8_t GrammarProfile::MarkupTraits(const std::string_view Name) {
	// 文法の既定の要素分類
	static constexpr std::string_view Blocks[] = {
		"address",
		"article",
		"aside",
		"blockquote",
		"body",
		"caption",
		"dd",
		"details",
		"dialog",
		"div",
		"dl",
		"dt",
		"fieldset",
		"figcaption",
		"figure",
		"footer",
		"form",
		"h1",
		"h2",
		"h3",
		"h4",
		"h5",
		"h6",
		"head",
		"header",
		"hgroup",
		"hr",
		"html",
		"li",
		"main",
		"menu",
		"nav",
		"ol",
		"p",
		"pre",
		"section",
		"summary",
		"table",
		"tbody",
		"td",
		"tfoot",
		"th",
		"thead",
		"tr",
		"ul"
	};
	static constexpr std::string_view HiddenNames[] = { "base", "head", "link", "meta", "script", "style", "template", "title" };
	static constexpr std::string_view Voids[] =
	{ "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr" };
	static constexpr std::string_view RawNames[] = { "iframe", "noembed", "noframes", "plaintext", "textarea", "title", "xmp" };
	static constexpr std::string_view Graphics[] =
	{ "circle", "defs", "ellipse", "g", "line", "math", "path", "polygon", "polyline", "rect", "svg" };
	// 要素名に対応する表示と内容の性質の合成
	uint8_t Result = Inline;
	if(std::ranges::find(Blocks, Name) != std::end(Blocks) || std::ranges::find(Graphics, Name) != std::end(Graphics)) {
		Result |= Block;
	}
	if(std::ranges::find(HiddenNames, Name) != std::end(HiddenNames)) Result |= MarkupTrait::Hidden;
	if(std::ranges::find(Voids, Name) != std::end(Voids)) Result |= Void;
	if(std::ranges::find(RawNames, Name) != std::end(RawNames)) Result |= Preserve | MarkupTrait::Raw;
	if(Name == "pre" || Name == "listing") Result |= Preserve;
	if(Names::HtmlKnownTag.Contains(Name)) Result |= Scanned;
	if(Name == "svg" || Name == "math") Result |= Foreign;
	if(Name == "text" || Name == "tspan" || Name == "mi" || Name == "mo" || Name == "mn") Result |= Text;
	// 性質の集合の返戻
	return Result;
}
