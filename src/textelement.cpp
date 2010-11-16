/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * textelement.cpp: 
 *
 * Contact:
 *   Moonlight List (moonlight-list@lists.ximian.com)
 *
 * Copyright 2010 Novell, Inc. (http://www.novell.com)
 *
 * See the LICENSE file included with the distribution for details.
 */

#include <config.h>

#include <cairo.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "textelement.h"
#include "deployment.h"
#include "runtime.h"
#include "color.h"
#include "utils.h"
#include "debug.h"
#include "uri.h"
#include "factory.h"
#include "enums.h"

namespace Moonlight {

//
// IDocumentNode
//

IDocumentNode*
IDocumentNode::CastToIDocumentNode (DependencyObject *obj)
{
	if (obj->Is (Type::RICHTEXTBOX))
		return (RichTextBox*)obj;
	else /* obj is a TextElement */
		return (TextElement*)obj;
}

//
// TextElement
//

TextElement::TextElement ()
{
	SetObjectType (Type::TEXTELEMENT);

	providers.inherited = new InheritedPropertyValueProvider (this, PropertyPrecedence_Inherited);

	font = new TextFontDescription ();
	downloaders = g_ptr_array_new ();
	text_pointers = g_ptr_array_new ();

	UpdateFontDescription (true);
}

TextElement::~TextElement ()
{
	CleanupDownloaders ();
	g_ptr_array_free (downloaders, TRUE);
	g_ptr_array_free (text_pointers, TRUE);
	delete font;
}

void
TextElement::NotifyLayoutContainerOnPropertyChanged (PropertyChangedEventArgs *args)
{
	DependencyObject *el = this;
	while (el) {
		if (el->Is (Type::TEXTBLOCK)) {
			((TextBlock*)el)->DocumentPropertyChanged (this, args);
			return;
		}
		else if (el->Is (Type::RICHTEXTBOX)) {
			((RichTextBox*)el)->GetView()->DocumentPropertyChanged (this, args);
			return;
		}
		else {
			el = el->GetParent(); // move to the collection
			if (el) el = el->GetParent(); // if the collection is there, move to its parent
		}
	}
}

void
TextElement::NotifyLayoutContainerOnCollectionChanged (Collection *col, CollectionChangedEventArgs *args)
{
	DependencyObject *el = this;
	while (el) {
		if (el->Is (Type::TEXTBLOCK)) {
			((TextBlock*)el)->DocumentCollectionChanged (this, col, args);
			return;
		}
		else if (el->Is (Type::RICHTEXTBOX)) {
			RichTextBoxView* view = ((RichTextBox*)el)->GetView();
			if (view)
				view->DocumentCollectionChanged (this, col, args);
			return;
		}
		else {
			el = el->GetParent(); // move to the collection
			if (el) el = el->GetParent(); // if the collection is there, move to its parent
		}
	}
}

bool
TextElement::LocalValueOverrides (int prop_id)
{
	Value *local = ReadLocalValue (prop_id);
	Value *lower_prec = GetValue (prop_id, (PropertyPrecedence)(PropertyPrecedence_LocalValue + 1));

	if (!local)
		return false;

	if (local && lower_prec) {
		return *local != *lower_prec;
	}
	else {
		return true;
	}
}

void
TextElement::CleanupDownloaders ()
{
	Downloader *downloader;
	guint i;
	
	for (i = 0; i < downloaders->len; i++) {
		downloader = (Downloader *) downloaders->pdata[i];
		downloader->RemoveHandler (Downloader::CompletedEvent, downloader_complete, this);
		downloader->Abort ();
		downloader->unref ();
	}
	
	g_ptr_array_set_size (downloaders, 0);
}

void
TextElement::downloader_complete (EventObject *sender, EventArgs *calldata, gpointer closure)
{
	((TextElement *) closure)->DownloaderComplete ((Downloader *) sender);
}

void
TextElement::DownloaderComplete (Downloader *downloader)
{
	FontManager *manager = Deployment::GetCurrent ()->GetFontManager ();
	const char *path;
	const Uri *uri;
	
	uri = downloader->GetUri ();
	
	// If the downloaded file was a zip file, this'll get the path to the
	// extracted zip directory, else it will simply be the path to the
	// downloaded file.
	if (!(path = downloader->GetUnzippedPath ()))
		return;
	
	manager->AddResource (uri->ToString (), path);
}

bool
TextElement::CoerceTextDecorations (DependencyObject *obj, DependencyProperty *p, Value *value, Value **coerced, MoonError *error)
{
	if (!value || value->GetIsNull())
		*coerced = new Value (TextDecorationsNone, Type::TEXTDECORATIONS);
	else
		*coerced = new Value (*value);

	return true;
}

void
TextElement::AddFontSource (Downloader *downloader)
{
	downloader->AddHandler (downloader->CompletedEvent, downloader_complete, this);
	g_ptr_array_add (downloaders, downloader);
	downloader->ref ();
	
	if (downloader->Started () || downloader->Completed ()) {
		if (downloader->Completed ())
			DownloaderComplete (downloader);
	} else {
		// This is what actually triggers the download
		downloader->Send ();
	}
}

void
TextElement::AddFontResource (const char *resource)
{
	FontManager *manager = Deployment::GetCurrent ()->GetFontManager ();
	Application *application = Application::GetCurrent ();
	Downloader *downloader;
	char *path;
	Uri *uri;
	
	uri = Uri::Create (resource);
	
	if (!uri)
		return;

	if (!application || !(path = application->GetResourceAsPath (GetResourceBase(), uri))) {
		if (IsAttached () && (downloader = GetDeployment ()->CreateDownloader ())) {
			downloader->Open ("GET", resource, FontPolicy);
			AddFontSource (downloader);
			downloader->unref ();
		}
		
		delete uri;
		
		return;
	}
	
	manager->AddResource (resource, path);
	g_free (path);
	delete uri;
}

void
TextElement::OnPropertyChanged (PropertyChangedEventArgs *args, MoonError *error)
{
	if (args->GetProperty ()->GetOwnerType () != Type::TEXTELEMENT) {
		DependencyObject::OnPropertyChanged (args, error);
		return;
	}
	
	if (args->GetId () == TextElement::FontFamilyProperty) {
		FontFamily *family = args->GetNewValue () ? args->GetNewValue ()->AsFontFamily () : NULL;
		char **families, *fragment;
		int i;
		
		CleanupDownloaders ();
		
		if (family && family->source) {
			families = g_strsplit (family->source, ",", -1);
			for (i = 0; families[i]; i++) {
				g_strstrip (families[i]);
				if ((fragment = strchr (families[i], '#'))) {
					// the first portion of this string is the resource name...
					*fragment = '\0';
					AddFontResource (families[i]);
				}
			}
			g_strfreev (families);
		}
		
		if (UpdateFontDescription (false))
			/*dirty = true*/;
	} else if (args->GetId () == TextElement::FontSizeProperty) {
		if (UpdateFontDescription (false))
			/*dirty = true*/;
	} else if (args->GetId () == TextElement::FontStretchProperty) {
		if (UpdateFontDescription (false))
			/*dirty = true*/;
	} else if (args->GetId () == TextElement::FontStyleProperty) {
		if (UpdateFontDescription (false))
			/*dirty = true*/;
	} else if (args->GetId () == TextElement::FontWeightProperty) {
		if (UpdateFontDescription (false))
			/*dirty = true*/;
	}
	
	NotifyListenersOfPropertyChange (args, error);
}

void
TextElement::OnSubPropertyChanged (DependencyProperty *prop, DependencyObject *obj, PropertyChangedEventArgs *subobj_args)
{
	if (prop && prop->GetId () == TextElement::ForegroundProperty) {
		// this isn't exactly what we want, I don't
		// think... but it'll have to do.
		NotifyListenersOfPropertyChange (prop, NULL);
	} else {
		DependencyObject::OnSubPropertyChanged (prop, obj, subobj_args);
	}
}

bool
TextElement::UpdateFontDescription (bool force)
{
	FontFamily *family = GetFontFamily ();
	bool changed = false;
	
	if (font->SetResource (GetFontResource()))
		changed = true;
	
	if (font->SetFamily (family ? family->source : NULL))
		changed = true;
	
	if (font->SetStretch (GetFontStretch ()->stretch))
		changed = true;
	
	if (font->SetWeight (GetFontWeight ()->weight))
		changed = true;
	
	if (font->SetStyle (GetFontStyle ()->style))
		changed = true;
	
	if (font->SetSize (GetFontSize ()))
		changed = true;
	
	if (font->SetLanguage (GetLanguage ()))
		changed = true;
	
	if (force) {
		font->Reload ();
		return true;
	}
	
	return changed;
}


void
TextElement::AddTextPointer (TextPointer *pointer)
{
	g_ptr_array_insert_sorted (text_pointers, TextPointer::Comparer, pointer);
}

void
TextElement::RemoveTextPointer (TextPointer *pointer)
{
	g_ptr_array_remove (text_pointers, pointer);
}

TextPointer*
TextElement::GetContentStart ()
{
	return new TextPointer (this, 0, LogicalDirectionBackward);
}

TextPointer
TextElement::GetContentStart_np ()
{
	return TextPointer (this, 0, LogicalDirectionBackward);
}

TextPointer*
TextElement::GetContentEnd ()
{
	return new TextPointer (this, -1, LogicalDirectionForward);
}

TextPointer
TextElement::GetContentEnd_np ()
{
	return TextPointer (this, -1, LogicalDirectionForward);
}

TextPointer*
TextElement::GetElementStart ()
{
	DependencyObject *el = GetParent();
	if (el) el = el->GetParent();

	if (!el)
		return NULL;

	return new TextPointer (el,
				IDocumentNode::CastToIDocumentNode (el)->GetDocumentChildren ()->IndexOf (Value (this)),
				LogicalDirectionBackward);
}

TextPointer*
TextElement::GetElementEnd ()
{
	DependencyObject *el = GetParent();
	if (el) el = el->GetParent();

	if (!el)
		return NULL;

	return new TextPointer (el,
				IDocumentNode::CastToIDocumentNode (el)->GetDocumentChildren ()->IndexOf (Value (this)),
				LogicalDirectionForward);
}

void
TextElement::SerializeProperties (bool force, GString *str)
{
	if (force || LocalValueOverrides (FontSizeProperty))
		g_string_append_printf (str, "FontSize=\"%g\" ", GetFontSize());
	if (force || LocalValueOverrides (FontFamilyProperty))
		g_string_append_printf (str, "FontFamily=\"%s\" ", GetFontFamily()->source);
	if (force || LocalValueOverrides (ForegroundProperty)) {
		if (GetForeground()->Is (Type::SOLIDCOLORBRUSH)) {
			g_string_append_printf (str, "Foreground=\"%s\" ", color_to_string (((SolidColorBrush*)GetForeground())->GetColor()));
		}
	}
	if (force || LocalValueOverrides (FontWeightProperty))
		g_string_append_printf (str, "FontWeight=\"%s\" ", enums_int_to_str ("FontWeight", GetFontWeight()->weight));
	if (force || LocalValueOverrides (FontStyleProperty))
		g_string_append_printf (str, "FontStyle=\"%s\" ", enums_int_to_str ("FontStyle", GetFontStyle()->style));
	if (force || LocalValueOverrides (FontStretchProperty))
		g_string_append_printf (str, "FontStretch=\"%s\" ", enums_int_to_str ("FontStretch", GetFontStretch()->stretch));
}

//
// Inline
//

Inline::Inline ()
{
	SetObjectType (Type::INLINE);
	autogen = false;
}

bool
Inline::Equals (Inline *item)
{
	const char *lang0, *lang1;
	
	if (item->GetObjectType () != GetObjectType ())
		return false;
	
	if (*item->GetFontFamily () != *GetFontFamily ())
		return false;
	
	if (item->GetFontSize () != GetFontSize ())
		return false;
	
	if (item->GetFontStyle () != GetFontStyle ())
		return false;
	
	if (item->GetFontWeight () != GetFontWeight ())
		return false;
	
	if (item->GetFontStretch () != GetFontStretch ())
		return false;
	
	if (item->GetTextDecorations () != GetTextDecorations ())
		return false;
	
	lang0 = item->GetLanguage ();
	lang1 = GetLanguage ();
	
	if ((lang0 && !lang1) || (!lang0 && lang1))
		return false;
	
	if (lang0 && lang1 && strcmp (lang0, lang1) != 0)
		return false;
	
	// this isn't really correct - we should be checking
	// the innards of the foreground brushes, but we're
	// guaranteed to never have a false positive here.
	if (item->GetForeground () != GetForeground ())
		return false;
	
	// OK, as best we can tell - they are equal
	return true;
}


//
// LineBreak
//

LineBreak::LineBreak ()
{
	SetObjectType (Type::LINEBREAK);
}

char*
LineBreak::Serialize ()
{
	GString *str = g_string_new ("<LineBreak ");
	SerializeProperties (false, str);
	g_string_append (str, "/>");
	return g_string_free (str, FALSE);
}

//
// Run
//

Run::Run ()
{
	SetObjectType (Type::RUN);
}

void
Run::OnPropertyChanged (PropertyChangedEventArgs *args, MoonError *error)
{
	if (args->GetProperty ()->GetOwnerType () != Type::RUN) {
		Inline::OnPropertyChanged (args, error);
		return;
	}

	if (args->GetId () == Run::TextProperty) {
		NotifyLayoutContainerOnPropertyChanged (args);
	}

	NotifyListenersOfPropertyChange (args, error);
}
 
bool
Run::Equals (Inline *item)
{
	const char *text, *itext;
	
	if (!Inline::Equals (item))
		return false;
	
	itext = ((Run *) item)->GetText ();
	text = GetText ();
	
	// compare the text
	if (text && itext && strcmp (text, itext) != 0)
		return false;
	else if ((text && !itext) || (!text && itext))
		return false;
	
	return true;
}

void
Run::SerializeProperties (bool force, GString *str)
{
	Inline::SerializeProperties (force, str);

	if (force || GetPropertyValueProvider (Deployment::GetCurrent()->GetTypes()->GetProperty(FlowDirectionProperty)) == PropertyPrecedence_LocalValue)
		g_string_append_printf (str, "FlowDirection=\"%s\" ", enums_int_to_str ("FlowDirection", GetFlowDirection()));

	g_string_append_printf (str, "Text=\"%s\" ", GetText ());
}

char*
Run::Serialize ()
{
	GString *str = g_string_new ("<Run ");
	SerializeProperties (false, str);
	g_string_append (str, "/>");
	return g_string_free (str, FALSE);
}

bool
Run::CoerceText (DependencyObject *obj, DependencyProperty *p, Value *value, Value **coerced, MoonError *error)
{
	if (!value || value->GetIsNull())
		*coerced = new Value ("");
	else
		*coerced = new Value (*value);

	return true;
}


//
// Block
//

Block::Block ()
{
	SetObjectType (Type::BLOCK);
}

void
Block::SerializeProperties (bool force, GString *str)
{
	TextElement::SerializeProperties (force, str);

	if (force || LocalValueOverrides (TextAlignmentProperty))
		g_string_append_printf (str, "TextAlignment=\"%s\" ", enums_int_to_str ("TextAlignment", GetTextAlignment()));
}


//
// Paragraph
//

Paragraph::Paragraph ()
{
	SetObjectType (Type::PARAGRAPH);
}

void
Paragraph::OnCollectionChanged (Collection *col, CollectionChangedEventArgs *args)
{
	if (PropertyHasValueNoAutoCreate (Paragraph::InlinesProperty, col)) {
		if (args->GetChangedAction () == CollectionChangedActionAdd)
			providers.inherited->PropagateInheritedPropertiesOnAddingToTree (args->GetNewItem()->AsInline());

		NotifyLayoutContainerOnCollectionChanged (col, args);
	} else {
		Block::OnCollectionChanged (col, args);
	}
}

char*
Paragraph::Serialize ()
{
	const char *trailer = "</Paragraph>";

	GString* str = g_string_new ("<Paragraph ");

	SerializeProperties (true, str);

	g_string_append (str, "/>");

	int c = GetInlines()->GetCount();
	for (int i = 0; i < c; i ++) {
		Inline *il = GetInlines()->GetValueAt(i)->AsInline();
		char *il_str = il->Serialize();
		g_string_append (str, il_str);
		g_free (il_str);
	}
	g_string_append (str, "</Paragraph>");
	return g_string_free (str, FALSE);
}

//
// Section
//

Section::Section ()
{
	SetObjectType (Type::SECTION);
}

char*
Section::Serialize ()
{
	const char *header = "<Section>";
	const char *trailer = "</Section>";

	GString* str = g_string_new (header);
	int c = GetBlocks()->GetCount();
	for (int i = 0; i < c; i ++) {
		Block *b = GetBlocks()->GetValueAt(i)->AsBlock();
		char *b_str = b->Serialize();
		g_string_append (str, b_str);
		g_free (b_str);
	}
	g_string_append (str, trailer);
	return g_string_free (str, FALSE);
}

void
Section::OnCollectionChanged (Collection *col, CollectionChangedEventArgs *args)
{
	if (PropertyHasValueNoAutoCreate (Section::BlocksProperty, col)) {
		if (args->GetChangedAction () == CollectionChangedActionAdd)
			providers.inherited->PropagateInheritedPropertiesOnAddingToTree (args->GetNewItem()->AsBlock());

		NotifyLayoutContainerOnCollectionChanged (col, args);
	} else {
		Block::OnCollectionChanged (col, args);
	}
}

//
// Span
//

Span::Span ()
{
	SetObjectType (Type::SPAN);
}

void
Span::OnCollectionChanged (Collection *col, CollectionChangedEventArgs *args)
{
	if (PropertyHasValueNoAutoCreate (Span::InlinesProperty, col)) {
		if (args->GetChangedAction () == CollectionChangedActionAdd)
			providers.inherited->PropagateInheritedPropertiesOnAddingToTree (args->GetNewItem()->AsInline());

		NotifyLayoutContainerOnCollectionChanged (col, args);
	} else {
		Inline::OnCollectionChanged (col, args);
	}
}

Value* 
Span::CreateInlineCollection (Type::Kind kind, DependencyProperty *property, DependencyObject *forObj)
{
	InlineCollection *col = MoonUnmanagedFactory::CreateInlineCollection ();
	if (forObj->Is (Type::HYPERLINK))
		col->SetIsForHyperlink(); // yuck...
	return Value::CreateUnrefPtr (col);
}

//
// Bold
//

Bold::Bold ()
{
	SetObjectType (Type::BOLD);
	
	// Yes, this is supposed to be locally set in the ctor.
	FontWeight weight (FontWeightsBold);
	SetFontWeight (&weight);
}

char*
Bold::Serialize ()
{
	const char *header = "<Bold>";
	const char *trailer = "</Bold>";

	GString* str = g_string_new (header);
	int c = GetInlines()->GetCount();
	for (int i = 0; i < c; i ++) {
		Inline *il = GetInlines()->GetValueAt(i)->AsInline();
		char *il_str = il->Serialize();
		g_string_append (str, il_str);
		g_free (il_str);
	}
	g_string_append (str, trailer);
	return g_string_free (str, FALSE);
}

//
// Italic
//

Italic::Italic ()
{
	SetObjectType (Type::ITALIC);
	
	FontStyle style (FontStylesItalic);
	SetFontStyle (&style);
}

char*
Italic::Serialize ()
{
	const char *header = "<Italic>";
	const char *trailer = "</Italic>";

	GString* str = g_string_new (header);
	int c = GetInlines()->GetCount();
	for (int i = 0; i < c; i ++) {
		Inline *il = GetInlines()->GetValueAt(i)->AsInline();
		char *il_str = il->Serialize();
		g_string_append (str, il_str);
		g_free (il_str);
	}
	g_string_append (str, trailer);
	return g_string_free (str, FALSE);
}

//
// Underline
//

Underline::Underline ()
{
	SetObjectType (Type::UNDERLINE);
	SetTextDecorations (TextDecorationsUnderline);
}

char*
Underline::Serialize ()
{
	const char *header = "<Underline>";
	const char *trailer = "</Underline>";

	GString* str = g_string_new (header);
	int c = GetInlines()->GetCount();
	for (int i = 0; i < c; i ++) {
		Inline *il = GetInlines()->GetValueAt(i)->AsInline();
		char *il_str = il->Serialize();
		g_string_append (str, il_str);
		g_free (il_str);
	}
	g_string_append (str, trailer);
	return g_string_free (str, FALSE);
}

//
// Hyperlink
//

Hyperlink::Hyperlink ()
{
	SetObjectType (Type::HYPERLINK);
	SetTextDecorations (TextDecorationsUnderline);

	// XXX these should happen here, but since they're inherited
	// properties, they end up causing a boatload of problems with
	// our forcing of managed refs.  They've been moved to
	// Hyperlink.Initialize in managed code.
	//
	// SetMouseOverForeground (new SolidColorBrush ("black"));
	// SetForeground (new SolidColorBrush ("#FF337CBB"));
}

void
Hyperlink::SerializeProperties (bool force, GString *str)
{
	Span::SerializeProperties (force, str);

	// is this something that's actually set by the hyperlink class?
	g_string_append (str, "TextDecorations=\"Underline\" ");

	if (force || GetPropertyValueProvider (Deployment::GetCurrent()->GetTypes()->GetProperty(TargetNameProperty)) == PropertyPrecedence_LocalValue)
		g_string_append_printf (str, "TargetName=\"%s\" ", GetTargetName ());
	g_string_append_printf (str, "NavigateUri=\"%s\" ", GetNavigateUri()->GetOriginalString()); // FIXME do we check for null uri here?
	if (force || LocalValueOverrides (MouseOverForegroundProperty)) {
		if (GetMouseOverForeground()->Is (Type::SOLIDCOLORBRUSH)) {
			g_string_append_printf (str, "MouseOverForeground=\"%s\" ", color_to_string (((SolidColorBrush*)GetMouseOverForeground())->GetColor()));
		}
	}
}

char*
Hyperlink::Serialize ()
{
	GString* str = g_string_new ("<Hyperlink ");
	SerializeProperties (false, str);
	g_string_append (str, "/>");
	int c = GetInlines()->GetCount();
	for (int i = 0; i < c; i ++) {
		Inline *il = GetInlines()->GetValueAt(i)->AsInline();
		char *il_str = il->Serialize();
		g_string_append (str, il_str);
		g_free (il_str);
	}
	g_string_append (str, "</Hyperlink>");
	return g_string_free (str, FALSE);
}


//
// InlineUIContainer
//

InlineUIContainer::InlineUIContainer ()
{
	SetObjectType (Type::INLINEUICONTAINER);
}

char*
InlineUIContainer::Serialize ()
{
	return g_strdup ("<Run Text=\"\"/>");
}

void
InlineUIContainer::OnPropertyChanged (PropertyChangedEventArgs *args, MoonError *error)
{
	if (args->GetProperty ()->GetOwnerType () != Type::INLINEUICONTAINER) {
		TextElement::OnPropertyChanged (args, error);
		return;
	}

	if (args->GetId () == InlineUIContainer::ChildProperty) {
		if (args->GetOldValue () && args->GetOldValue()->AsUIElement ()) {
			if (args->GetOldValue()->Is(GetDeployment (), Type::FRAMEWORKELEMENT)) {
				args->GetOldValue()->AsFrameworkElement()->SetLogicalParent (NULL, error);
				if (error->number)
					return;
			}
		}

		if (args->GetNewValue() && args->GetNewValue()->AsUIElement()) {
			if (args->GetNewValue()->Is(GetDeployment (), Type::FRAMEWORKELEMENT)) {
				FrameworkElement *fwe = args->GetNewValue()->AsFrameworkElement ();
				if (fwe->GetLogicalParent() && fwe->GetLogicalParent() != this) {
					MoonError::FillIn (error, MoonError::ARGUMENT, "Element is already a child of another element");
					return;
				}

				args->GetNewValue()->AsFrameworkElement()->SetLogicalParent (this, error);
				if (error->number)
					return;
			}
		}

		NotifyLayoutContainerOnPropertyChanged (args);
	}

	NotifyListenersOfPropertyChange (args, error);
}

};
