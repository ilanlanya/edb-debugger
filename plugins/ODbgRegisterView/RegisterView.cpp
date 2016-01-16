/*
Copyright (C) 2015 Ruslan Kabatsayev <b7.10110111@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "RegisterView.h"
#include <QMouseEvent>
#include <QLabel>
#include <QApplication>
#include <QMessageBox>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QVBoxLayout>
#include <QPlastiqueStyle>
#include <QAction>
#include <QMenu>
#include <QSignalMapper>
#include <QSettings>
#include <QClipboard>
#include <algorithm>
#include <unordered_map>
#include <QDebug>
#include <iostream>
#include "RegisterViewModelBase.h"
#include "edb.h"
#include "Configuration.h"
#include "State.h"
#include "DialogEditGPR.h"
#include "DialogEditSIMDRegister.h"
#include "DialogEditFPU.h"

#define VALID_VARIANT(VARIANT) (Q_ASSERT((VARIANT).isValid()),(VARIANT))
#define VALID_INDEX(INDEX) VALID_VARIANT(INDEX)
#define CHECKED_CAST(TYPE,PARENT) (Q_ASSERT(dynamic_cast<TYPE*>(PARENT)),static_cast<TYPE*>(PARENT))

namespace ODbgRegisterView {

// TODO: Right click => select too
// TODO: Enter key => modify/toggle
// TODO: GPR menu: Increment, Decrement, Invert, Zero(if not already), Set to 1(if not already)
// TODO: rFLAGS menu: Set Condition (O,P,NAE etc. - see ODB)
// TODO: FPU tags: toggle - set valid/empty
// TODO: FSR: Set Condition: G,L,E,Unordered
// TODO: PC: set 24/53/64-bit mantissa
// TODO: RC: round up/down/nearest
// TODO: Push/Pop FPU stack
// TODO: Add option to show FPU in STi mode, both ST-ordered and R-ordered (physically)
// TODO: Update register comments after editing values

namespace
{

const constexpr auto registerGroupTypeNames=util::make_array<const char*>(
			"GPR",
			"rIP",
			"ExpandedEFL",
			"Segment",
			"EFL",
			"FPUData",
			"FPUWords",
			"FPULastOp",
			"Debug",
			"MMX",
			"SSEData",
			"AVXData",
			"MXCSR"
		);
static_assert(registerGroupTypeNames.size()==ODBRegView::RegisterGroupType::NUM_GROUPS,
					"Mismatch between number of register group types and names");

const QString SETTINGS_GROUPS_ARRAY_NODE="visibleGroups";

static const int MODEL_NAME_COLUMN=RegisterViewModelBase::Model::NAME_COLUMN;
static const int MODEL_VALUE_COLUMN=RegisterViewModelBase::Model::VALUE_COLUMN;
static const int MODEL_COMMENT_COLUMN=RegisterViewModelBase::Model::COMMENT_COLUMN;

template<typename T>
inline T sqr(T v) { return v*v; }

inline QPoint fieldPos(const FieldWidget* const field)
{
	// NOTE: mapToGlobal() is VERY slow, don't use it. Here we map to canvas, it's enough for all fields.
	return field->mapTo(field->parentWidget()->parentWidget(),QPoint());
}

// Square of Euclidean distance between two points
inline int distSqr(QPoint const& w1, QPoint const& w2)
{
	return sqr(w1.x()-w2.x())+sqr(w1.y()-w2.y());
}

inline QSize letterSize(QFont const& font)
{
	const QFontMetrics fontMetrics(font);
	const int width=fontMetrics.width('w');
	const int height=fontMetrics.height();
	return QSize(width,height);
}

QAction* newActionSeparator(QObject* parent)
{
	const auto sep=new QAction(parent);
	sep->setSeparator(true);
	return sep;
}

QAction* newAction(QString const& text, QObject* parent, QObject* signalReceiver, const char* slot)
{
	const auto action=new QAction(text,parent);
	QObject::connect(action,SIGNAL(triggered()),signalReceiver,slot);
	return action;
}

QAction* newAction(QString const& text, QObject* parent, QSignalMapper* mapper, int mapping)
{
	const auto action=newAction(text,parent,mapper,SLOT(map()));
	mapper->setMapping(action,mapping);
	return action;
}

static QPlastiqueStyle plastiqueStyle;

}

// --------------------- FieldWidget impl ----------------------------------
QString FieldWidget::text() const
{
	if(!index.isValid() && !this->isEnabled())
		return QLabel::text();
	const auto text=index.data();
	if(!text.isValid())
		return QString(width()/letterSize(font()).width()-1,QChar('?'));
	return text.toString();
}

int FieldWidget::lineNumber() const
{
	const auto charSize=letterSize(font());
	return fieldPos(this).y()/charSize.height();
}

int FieldWidget::columnNumber() const
{
	const auto charSize=letterSize(font());
	return fieldPos(this).x()/charSize.width();
}

void FieldWidget::init(int const fieldWidth)
{
	setObjectName("FieldWidget");
	const auto charSize=letterSize(font());
	setFixedHeight(charSize.height());
	if(fieldWidth>0)
		setFixedWidth(fieldWidth*charSize.width());
	setDisabled(true);
}

FieldWidget::FieldWidget(int const fieldWidth, QModelIndex const& index, QWidget* const parent)
	: QLabel("Fw???",parent),
	  index(index),
	  fieldWidth_(fieldWidth)
{
	init(fieldWidth);
}

FieldWidget::FieldWidget(int const fieldWidth, QString const& fixedText, QWidget* const parent)
	: QLabel(fixedText,parent),
	  fieldWidth_(fieldWidth)
{
	init(fieldWidth); // NOTE: fieldWidth!=fixedText.length() in general
}

FieldWidget::FieldWidget(QString const& fixedText, QWidget* const parent)
	: QLabel(fixedText,parent),
	  fieldWidth_(fixedText.length())
{
	init(fixedText.length());
}

int FieldWidget::fieldWidth() const
{
	return fieldWidth_;
}

void FieldWidget::update()
{
	QLabel::setText(text());
	adjustSize();
}

ODBRegView* FieldWidget::regView() const
{
	auto* const parent=parentWidget() // group
					 ->parentWidget() // canvas
					 ->parentWidget() // viewport
					 ->parentWidget(); // regview
	Q_ASSERT(dynamic_cast<ODBRegView*>(parent));
	return static_cast<ODBRegView*>(parent);
}

RegisterGroup* FieldWidget::group() const
{
	return CHECKED_CAST(RegisterGroup,parentWidget());
}

VolatileNameField::VolatileNameField(int fieldWidth,
									 std::function<QString()> const& valueFormatter,
									 QWidget* parent)
	: FieldWidget(fieldWidth,"",parent),
	  valueFormatter(valueFormatter)
{
}

QString VolatileNameField::text() const
{
	return valueFormatter();
}

// --------------------- ValueField impl ----------------------------------
ValueField::ValueField(int const fieldWidth,
					   QModelIndex const& index,
					   QWidget* const parent,
					   std::function<QString(QString const&)> const& valueFormatter
					  )
	: FieldWidget(fieldWidth,index,parent),
	  valueFormatter(valueFormatter)
{
	setObjectName("ValueField");
	setDisabled(false);
	setMouseTracking(true);
	// Set some known style to avoid e.g. Oxygen's label transition animations, which
	// break updating of colors such as "register changed" when single-stepping frequently
	setStyle(&plastiqueStyle);

	using namespace RegisterViewModelBase;
	if(index.data(Model::IsNormalRegisterRole).toBool())
		menuItems.push_back(newAction(tr("Modify"),this,this,SLOT(defaultAction()))); // TODO: implement
	else if(index.data(Model::IsBitFieldRole).toBool() && index.data(Model::BitFieldLengthRole).toInt()==1)
		menuItems.push_back(newAction(tr("Toggle"),this,this,SLOT(defaultAction())));
}

RegisterViewModelBase::Model* ValueField::model() const
{
	using namespace RegisterViewModelBase;
	const auto model=static_cast<const Model*>(index.model());
	// The model is not supposed to have been created as const object,
	// and our manipulations won't invalidate the index.
	// Thus cast the const away.
	return const_cast<Model*>(model);
}

ValueField* ValueField::bestNeighbor(std::function<bool(QPoint const&,ValueField const*,QPoint const&)>const& firstIsBetter) const
{
	ValueField* result=nullptr;
	for(auto* const neighbor : regView()->valueFields())
	{
		if(neighbor->isVisible() && firstIsBetter(fieldPos(neighbor),result,fieldPos(this)))
			result=neighbor;
	}
	return result;
}

ValueField* ValueField::up() const
{
	return bestNeighbor([](QPoint const& nPos,ValueField const*up,QPoint const& fPos)
			{ return nPos.y() < fPos.y() && (!up || distSqr(nPos,fPos) < distSqr(fieldPos(up),fPos)); });
}

ValueField* ValueField::down() const
{
	return bestNeighbor([](QPoint const& nPos,ValueField const*down,QPoint const& fPos)
			{ return nPos.y() > fPos.y() && (!down || distSqr(nPos,fPos) < distSqr(fieldPos(down),fPos)); });
}

ValueField* ValueField::left() const
{
	return bestNeighbor([](QPoint const& nPos,ValueField const*left,QPoint const& fPos)
			{ return nPos.y()==fPos.y() && nPos.x()<fPos.x() && (!left || left->x()<nPos.x()); });
}

ValueField* ValueField::right() const
{
	return bestNeighbor([](QPoint const& nPos,ValueField const*right,QPoint const& fPos)
			{ return nPos.y()==fPos.y() && nPos.x()>fPos.x() && (!right || right->x()>nPos.x()); });
}

QString ValueField::text() const
{
	return valueFormatter(FieldWidget::text());
}

bool ValueField::changed() const
{
	if(!index.isValid()) return true;
	return VALID_VARIANT(index.data(RegisterViewModelBase::Model::RegisterChangedRole)).toBool();
}

QColor ValueField::fgColorForChangedField() const
{
	return Qt::red; // TODO: read from user palette
}

bool ValueField::isSelected() const
{
	return selected_;
}

void ValueField::editNormalReg(QModelIndex const& indexToEdit, QModelIndex const& clickedIndex) const
{
	using namespace RegisterViewModelBase;

	const auto rV=model()->data(indexToEdit,Model::ValueAsRegisterRole);
	if(!rV.isValid()) return;
	auto r=rV.value<Register>();
	if(!r) return;

	if((r.type()!=Register::TYPE_SIMD) && r.bitSize()<=64)
	{

		const auto gprEdit=regView()->gprEditDialog();
		gprEdit->set_value(r);
		if(gprEdit->exec()==QDialog::Accepted)
		{
			r=gprEdit->value();
			model()->setData(indexToEdit,QVariant::fromValue(r),Model::ValueAsRegisterRole);
		}
	}
	else if(r.type()==Register::TYPE_SIMD)
	{
		const auto simdEdit=regView()->simdEditDialog();
		simdEdit->set_value(r);
		const int size=VALID_VARIANT(indexToEdit.parent().data(Model::ChosenSIMDSizeRole)).toInt();
		const int format=VALID_VARIANT(indexToEdit.parent().data(Model::ChosenSIMDFormatRole)).toInt();
		const int elementIndex=clickedIndex.row();
		simdEdit->set_current_element(static_cast<Model::ElementSize>(size),
									static_cast<NumberDisplayMode>(format),
									elementIndex);
		if(simdEdit->exec()==QDialog::Accepted)
		{
			r=simdEdit->value();
			model()->setData(indexToEdit,QVariant::fromValue(r),Model::ValueAsRegisterRole);
		}
	}
	else if(r.type()==Register::TYPE_FPU)
	{
		const auto fpuEdit=regView()->fpuEditDialog();
		fpuEdit->set_value(r);
		if(fpuEdit->exec()==QDialog::Accepted)
		{
			r=fpuEdit->value();
			model()->setData(indexToEdit,QVariant::fromValue(r),Model::ValueAsRegisterRole);
		}
	}
}

void ValueField::defaultAction()
{
	using namespace RegisterViewModelBase;
	if(index.data(Model::IsBitFieldRole).toBool() && index.data(Model::BitFieldLengthRole).toInt()==1)
	{
		// toggle
		// TODO: Model: make it possible to set bit field itself, without manipulating parent directly
		//              I.e. set value without knowing field offset, then setData(fieldIndex,word)
		const auto regIndex=index.parent().sibling(index.parent().row(),MODEL_VALUE_COLUMN);
		auto byteArr=regIndex.data(Model::RawValueRole).toByteArray();
		if(byteArr.isEmpty()) return;
		std::uint64_t word(0);
		std::memcpy(&word,byteArr.constData(),byteArr.size());
		const auto offset=VALID_INDEX(index.data(Model::BitFieldOffsetRole)).toInt();
		word^=1ull<<offset;
		std::memcpy(byteArr.data(),&word,byteArr.size());
		model()->setData(regIndex,byteArr,Model::RawValueRole);
	}
	else if(index.data(Model::IsNormalRegisterRole).toBool())
		editNormalReg(index,index);
	else if(index.data(Model::IsSIMDElementRole).toBool())
		editNormalReg(index.parent().parent(), index);
	else if(index.parent().data(Model::IsFPURegisterRole).toBool())
		editNormalReg(index.parent(),index);
	else
		QMessageBox::information(this,"Unimplemented",
								 QString("Sorry, editing %1 is not implemented yet")
									.arg(index.sibling(index.row(),MODEL_NAME_COLUMN).data().toString()));
}

void ValueField::update()
{
	FieldWidget::update();
	updatePalette();
}

void ValueField::updatePalette()
{
	if(changed())
	{
		auto palette=this->palette();
		const QColor changedFGColor=fgColorForChangedField();
		palette.setColor(foregroundRole(),changedFGColor);
		palette.setColor(QPalette::HighlightedText,changedFGColor);
		setPalette(palette);
	}
	else setPalette(QApplication::palette());

	QLabel::update();
}

void ValueField::enterEvent(QEvent*)
{
	hovered_=true;
	updatePalette();
}

void ValueField::leaveEvent(QEvent*)
{
	hovered_=false;
	updatePalette();
}

void ValueField::select()
{
	if(selected_) return;
	selected_=true;
	Q_EMIT selected();
	updatePalette();
}

void ValueField::showMenu(QPoint const& position)
{
	group()->showMenu(position,menuItems);
}

void ValueField::mousePressEvent(QMouseEvent* event)
{
	if(event->button() & (Qt::LeftButton|Qt::RightButton))
		select();
	if(event->button()==Qt::RightButton && event->type()!=QEvent::MouseButtonDblClick)
		showMenu(event->globalPos());
}

void ValueField::unselect()
{
	if(!selected_) return;
	selected_=false;
	updatePalette();
}

void ValueField::mouseDoubleClickEvent(QMouseEvent* event)
{
	mousePressEvent(event);
	defaultAction();
}

void ValueField::paintEvent(QPaintEvent*)
{
	auto*const regView=this->regView();
	QPainter painter(this);
	QStyleOptionViewItemV4 option;
	option.rect=rect();
	option.showDecorationSelected=true;
	option.text=text();
	option.font=font();
	option.palette=palette();
	option.textElideMode=Qt::ElideNone;
	option.state |= QStyle::State_Enabled;
	option.displayAlignment=alignment();
	if(selected_) option.state |= QStyle::State_Selected;
	if(hovered_)  option.state |= QStyle::State_MouseOver;
	if(regView->hasFocus())
		option.state |= QStyle::State_Active;
	QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &option, &painter);
}

// -------------------------------- RegisterGroup impl ----------------------------
RegisterGroup::RegisterGroup(QString const& name, QWidget* parent)
	: QWidget(parent),
	  name(name)
{
	setObjectName("RegisterGroup_"+name);

	{
		menuItems.push_back(newActionSeparator(this));
		menuItems.push_back(newAction(tr("Hide %1","register group").arg(name),this,this,SLOT(hideAndReport())));
	}
}

void RegisterGroup::hideAndReport()
{
	hide();
	regView()->groupHidden(this);
}

void RegisterGroup::showMenu(QPoint const& position, QList<QAction*>const& additionalItems) const
{
	return regView()->showMenu(position,additionalItems+menuItems);
}

void RegisterGroup::mousePressEvent(QMouseEvent* event)
{
	if(event->button()==Qt::RightButton)
		showMenu(event->globalPos(),menuItems);
	else
		event->ignore();
}

ODBRegView* RegisterGroup::regView() const
{
	return CHECKED_CAST(ODBRegView,  parent() // canvas
								   ->parent() // viewport
								   ->parent() // regview
								   );
}

void RegisterGroup::insert(int const line, int const column, FieldWidget* const widget)
{
	widget->update();

	if(auto* const value=dynamic_cast<ValueField*>(widget))
		connect(value,SIGNAL(selected()),regView(),SLOT(fieldSelected()));

	const auto charSize=letterSize(font());
	const auto charWidth=charSize.width();
	const auto charHeight=charSize.height();
	// extra space for highlighting rectangle, so that single-digit fields are easier to target
	const auto marginLeft=charWidth/2;
	const auto marginRight=charWidth-marginLeft;

	QPoint position(charWidth*column,charHeight*line);
	position -= QPoint(marginLeft,0);

	QSize size(widget->size());
	size += QSize(marginLeft+marginRight,0);

	widget->setMinimumSize(size);
	widget->move(position);
	// FIXME: why are e.g. regnames like FSR truncated without the -1?
	widget->setContentsMargins(marginLeft,0,marginRight-1,0);

	const auto potentialNewWidth=widget->pos().x()+widget->width();
	const auto potentialNewHeight=widget->pos().y()+widget->height();
	const auto oldMinSize=minimumSize();
	if(potentialNewWidth > oldMinSize.width() || potentialNewHeight > oldMinSize.height())
		setMinimumSize(std::max(potentialNewWidth,oldMinSize.width()),std::max(potentialNewHeight,oldMinSize.height()));

	widget->show();
}

int RegisterGroup::lineAfterLastField() const
{
	const auto fields=this->fields();
	const auto bottomField=std::max_element(fields.begin(),fields.end(),
												[](FieldWidget* l,FieldWidget* r)
												{ return l->pos().y()<r->pos().y(); });
	return bottomField==fields.end() ? 0 : (*bottomField)->pos().y()/(*bottomField)->height()+1;
}

void RegisterGroup::appendNameValueComment(QModelIndex const& nameIndex, QString const& tooltip, bool insertComment)
{
	Q_ASSERT(nameIndex.isValid());
	using namespace RegisterViewModelBase;
	const auto nameWidth=nameIndex.data(Model::FixedLengthRole).toInt();
	Q_ASSERT(nameWidth>0);
	const auto valueIndex=nameIndex.sibling(nameIndex.row(),Model::VALUE_COLUMN);
	const auto valueWidth=valueIndex.data(Model::FixedLengthRole).toInt();
	Q_ASSERT(valueWidth>0);

	const int line=lineAfterLastField();
	int column=0;
	const auto nameField=new FieldWidget(nameWidth,nameIndex.data().toString(),this);
	insert(line,column,nameField);
	column+=nameWidth+1;
	const auto valueField=new ValueField(valueWidth,valueIndex,this);
	insert(line,column,valueField);
	if(!tooltip.isEmpty())
	{
		nameField->setToolTip(tooltip);
		valueField->setToolTip(tooltip);
	}
	if(insertComment)
	{
		column+=valueWidth+1;
		const auto commentIndex=nameIndex.sibling(nameIndex.row(),Model::COMMENT_COLUMN);
		insert(line,column,new FieldWidget(0,commentIndex,this));
	}
}

QList<FieldWidget*> RegisterGroup::fields() const
{
	const auto children=this->children();
	QList<FieldWidget*> fields;
	for(auto* const child : children)
	{
		const auto field=dynamic_cast<FieldWidget*>(child);
		if(field) fields.append(field);
	}
	return fields;
}

QList<ValueField*> RegisterGroup::valueFields() const
{
	QList<ValueField*> allValues;
	for(auto* const field : fields())
	{
		auto* const value=dynamic_cast<ValueField*>(field);
		if(value) allValues.push_back(value);
	}
	return allValues;
}

// ------------------------------- Canvas impl ----------------------------------------

Canvas::Canvas(QWidget* parent)
	: QWidget(parent)
{
	setObjectName("RegViewCanvas");
	auto* const canvasLayout=new QVBoxLayout(this);
	canvasLayout->setSpacing(letterSize(parent->font()).height()/2);
	canvasLayout->setContentsMargins(parent->contentsMargins());
	canvasLayout->setAlignment(Qt::AlignTop);
	setLayout(canvasLayout);
	setBackgroundRole(QPalette::Base);
	setAutoFillBackground(true);
}

void Canvas::mousePressEvent(QMouseEvent* event)
{
	event->ignore();
}

// -------------------------------- ODBRegView impl ----------------------------------------

void ODBRegView::mousePressEvent(QMouseEvent* event)
{
	if(event->type()!=QEvent::MouseButtonPress) return;

	if(event->button()==Qt::RightButton)
	{
		showMenu(event->globalPos());
		return;
	}

	if(event->button()==Qt::LeftButton)
		for(auto* const field : valueFields())
			field->unselect();
}

void ODBRegView::fieldSelected()
{
	for(auto* const field : valueFields())
		if(sender()!=field)
			field->unselect();
	ensureWidgetVisible(static_cast<QWidget*>(sender()),0,0);
}

void ODBRegView::showMenu(QPoint const& position, QList<QAction*>const& additionalItems) const
{
	QMenu menu;
	auto items=additionalItems+menuItems;
	for(const auto action : items)
		menu.addAction(action);

	menu.exec(position);
}

void RegisterGroup::adjustWidth()
{
	int widthNeeded=0;
	for(auto* const field : fields())
	{
		const auto widthToRequire=field->pos().x()+field->width();
		if(widthToRequire>widthNeeded) widthNeeded=widthToRequire;
	}
	setMinimumWidth(widthNeeded);
}

ODBRegView::RegisterGroupType findGroup(QString const& str)
{
	const auto& names=registerGroupTypeNames;
	const auto foundIt=std::find(names.begin(),names.end(),str);
	if(foundIt==names.end()) return ODBRegView::RegisterGroupType::NUM_GROUPS;
	return ODBRegView::RegisterGroupType(foundIt-names.begin());
}

ODBRegView::ODBRegView(QString const& settingsGroup, QWidget* parent)
	: QScrollArea(parent),
	  dialogEditGPR(new DialogEditGPR(this)),
	  dialogEditSIMDReg(new DialogEditSIMDRegister(this)),
	  dialogEditFPU(new DialogEditFPU(this))
{
	setObjectName("ODBRegView");

	{
		// TODO: get some signal to change font on the fly
		// NOTE: on getting this signal all the fields must be resized and moved
		QFont font;
		if(!font.fromString(edb::v1::config().registers_font))
		{
			font=QFont("Monospace");
			font.setStyleHint(QFont::TypeWriter);
		}
		setFont(font);
	}

	auto* const canvas=new Canvas(this);
	setWidget(canvas);
	setWidgetResizable(true);

	{
		const auto sep=new QAction(this);
		sep->setSeparator(true);
		menuItems.push_back(sep);
		menuItems.push_back(newAction(tr("Copy all registers"),this,this,SLOT(copyAllRegisters())));
	}

	QSettings settings;
	settings.beginGroup(settingsGroup);
	const auto groupListV=settings.value(SETTINGS_GROUPS_ARRAY_NODE);
	if(settings.group().isEmpty() || !groupListV.isValid())
	{
		visibleGroupTypes={
							RegisterGroupType::GPR,
							RegisterGroupType::rIP,
							RegisterGroupType::ExpandedEFL,
							RegisterGroupType::Segment,
							RegisterGroupType::EFL,
							RegisterGroupType::FPUData,
							RegisterGroupType::FPUWords,
							RegisterGroupType::FPULastOp,
							RegisterGroupType::Debug,
							RegisterGroupType::MMX,
							RegisterGroupType::SSEData,
							RegisterGroupType::AVXData,
							RegisterGroupType::MXCSR
						  };
	}
	else
	{
		for(const auto grp : groupListV.toStringList())
		{
			const auto group=findGroup(grp);
			if(group>=RegisterGroupType::NUM_GROUPS)
			{
				qWarning() << qPrintable(QString("Warning: failed to understand group %1").arg(group));
				continue;
			}
			visibleGroupTypes.emplace_back(group);
		}
	}
}

DialogEditGPR* ODBRegView::gprEditDialog() const
{
	return dialogEditGPR;
}

DialogEditSIMDRegister* ODBRegView::simdEditDialog() const
{
	return dialogEditSIMDReg;
}

DialogEditFPU* ODBRegView::fpuEditDialog() const
{
	return dialogEditFPU;
}

void ODBRegView::copyAllRegisters()
{
	auto allFields=fields();
	std::sort(allFields.begin(),allFields.end(),[](FieldWidget const*const f1, FieldWidget const*const f2)
			{
				const auto f1Pos=fieldPos(f1);
				const auto f2Pos=fieldPos(f2);
				if(f1Pos.y()<f2Pos.y()) return true;
				if(f1Pos.y()>f2Pos.y()) return false;
				return f1Pos.x()<f2Pos.x(); 
			});

	QString text;
	int textLine=0;
	int textColumn=0;
	for(const auto*const field : allFields)
	{
		while(field->lineNumber()>textLine)
		{
			++textLine;
			textColumn=0;
			text+='\n';
		}
		while(field->columnNumber()>textColumn)
		{
			++textColumn;
			text+=' ';
		}
		const QString fieldText=field->text();
		if(field->alignment()==Qt::AlignRight)
		{
			const int fwidth=field->fieldWidth();
			const int spaceWidth=fwidth-fieldText.length();
			text+=QString(spaceWidth,' ');
			textColumn+=spaceWidth;
		}
		text+=fieldText;
		textColumn+=fieldText.length();
	}
	QApplication::clipboard()->setText(text.trimmed());
}

void ODBRegView::groupHidden(RegisterGroup* group)
{
	using namespace std;
	Q_ASSERT(util::contains(groups,group));
	const auto groupPtrIter=std::find(groups.begin(),groups.end(),group);
	auto& groupPtr=*groupPtrIter;
	groupPtr->deleteLater();
	groupPtr=nullptr;

	auto& types(visibleGroupTypes);
	const int groupType=groupPtrIter-groups.begin();
	types.erase(remove_if(types.begin(),types.end(), [=](int type){return type==groupType;}) ,types.end());
}

void ODBRegView::saveState(QString const& settingsGroup) const
{
	QSettings settings;
	settings.beginGroup(settingsGroup);
	settings.remove(SETTINGS_GROUPS_ARRAY_NODE);
	QStringList groupTypes;
	for(auto type : visibleGroupTypes)
		groupTypes << registerGroupTypeNames[type];
	settings.setValue(SETTINGS_GROUPS_ARRAY_NODE,groupTypes);
}

void ODBRegView::setModel(RegisterViewModelBase::Model* model)
{
	model_=model;
	connect(model,SIGNAL(modelReset()),this,SLOT(modelReset()));
	connect(model,SIGNAL(dataChanged(QModelIndex const&,QModelIndex const&)),this,SLOT(modelUpdated()));
	modelReset();
}

namespace
{
// TODO: switch from string-based search to enum-based one (add a new Role to model data)
QModelIndex findModelCategory(RegisterViewModelBase::Model const*const model,
							  QString const& catToFind)
{
	for(int row=0;row<model->rowCount();++row)
	{
		const auto cat=model->index(row,0).data(MODEL_NAME_COLUMN);
		if(cat.isValid() && cat.toString()==catToFind)
			return model->index(row,0);
	}
	return QModelIndex();
}

// TODO: switch from string-based search to enum-based one (add a new Role to model data)
QModelIndex findModelRegister(QModelIndex categoryIndex,
							  QString const& regToFind,
							  int column=MODEL_NAME_COLUMN)
{
	auto* const model=categoryIndex.model();
	for(int row=0;row<model->rowCount(categoryIndex);++row)
	{
		const auto regIndex=model->index(row,MODEL_NAME_COLUMN,categoryIndex);
		const auto name=model->data(regIndex).toString();
		if(name.toUpper()==regToFind)
		{
			if(column==MODEL_NAME_COLUMN)
				return regIndex;
			return regIndex.sibling(regIndex.row(),column);
		}
	}
	return QModelIndex();
}

QModelIndex getCommentIndex(QModelIndex const& nameIndex)
{
	Q_ASSERT(nameIndex.isValid());
	return nameIndex.sibling(nameIndex.row(),MODEL_COMMENT_COLUMN);
}

QModelIndex getValueIndex(QModelIndex const& nameIndex)
{
	Q_ASSERT(nameIndex.isValid());
	return nameIndex.sibling(nameIndex.row(),MODEL_VALUE_COLUMN);
}

void addRoundingMode(RegisterGroup* const group, QModelIndex const& index, int const row, int const column)
{
	Q_ASSERT(index.isValid());
	const auto rndValueField=new ValueField(4,index,group,[](QString const& str)
				{
					Q_ASSERT(str.length());
					if(str[0]=='?') return "????";
					bool roundModeParseOK=false;
					const int value=str.toInt(&roundModeParseOK);
					if(!roundModeParseOK) return "????";
					Q_ASSERT(0<=value && value<=3);
					static const char* strings[]={"NEAR","DOWN","  UP","ZERO"};
					return strings[value];
				});
	group->insert(row,column,rndValueField);
	rndValueField->setToolTip(QObject::tr("Rounding mode"));
}

void addPrecisionMode(RegisterGroup* const group, QModelIndex const& index, int const row, int const column)
{
	Q_ASSERT(index.isValid());
	const auto precValueField=new ValueField(2,index,group,[](QString const& str)
				{
					Q_ASSERT(str.length());
					if(str[0]=='?') return "??";
					bool precModeParseOK=false;
					const int value=str.toInt(&precModeParseOK);
					if(!precModeParseOK) return "??";
					Q_ASSERT(0<=value && value<=3);
					static const char* strings[]={"24","??","53","64"};
					return strings[value];
				});
	group->insert(row,column,precValueField);
	precValueField->setToolTip(QObject::tr("Precision mode: effective mantissa length"));
}

void addPUOZDI(RegisterGroup* const group, QModelIndex const& excRegIndex, QModelIndex const& maskRegIndex, int const startRow, int const startColumn)
{
	QString const exceptions="PUOZDI";
	std::unordered_map<char,QString> excNames=
		{
		{'P',QObject::tr("Precision")},
		{'U',QObject::tr("Underflow")},
		{'O',QObject::tr("Overflow")},
		{'Z',QObject::tr("Zero Divide")},
		{'D',QObject::tr("Denormalized Operand")},
		{'I',QObject::tr("Invalid Operation")}
		};
	for(int exN=0;exN<exceptions.length();++exN)
	{
		QString const ex=exceptions[exN];
		const auto exAbbrev=ex+"E";
		const auto maskAbbrev=ex+"M";
		const auto excIndex=VALID_INDEX(findModelRegister(excRegIndex,exAbbrev));
		const auto maskIndex=VALID_INDEX(findModelRegister(maskRegIndex,maskAbbrev));
		const int column=startColumn+exN*2;
		const auto nameField=new FieldWidget(ex,group);
		group->insert(startRow,column,nameField);
		const auto excValueField=new ValueField(1,getValueIndex(excIndex),group);
		group->insert(startRow+1,column,excValueField);
		const auto maskValueField=new ValueField(1,getValueIndex(maskIndex),group);
		group->insert(startRow+2,column,maskValueField);

		const auto excName=excNames.at(ex[0].toLatin1());
		nameField->setToolTip(excName);
		excValueField->setToolTip(excName+' '+QObject::tr("Exception")+" ("+exAbbrev+")");
		maskValueField->setToolTip(excName+' '+QObject::tr("Exception Mask")+" ("+maskAbbrev+")");
	}
}

}

RegisterGroup* createEFL(RegisterViewModelBase::Model* model,QWidget* parent)
{
	const auto catIndex=findModelCategory(model,"General Status");
	if(!catIndex.isValid()) return nullptr;
	auto nameIndex=findModelRegister(catIndex,"RFLAGS");
	if(!nameIndex.isValid())
		nameIndex=findModelRegister(catIndex,"EFLAGS");
	if(!nameIndex.isValid()) return nullptr;
	auto* const group=new RegisterGroup("EFL",parent);
	const int nameWidth=3;
	int column=0;
	group->insert(0,column,new FieldWidget("EFL",group));
	const auto valueWidth=8;
	const auto valueIndex=nameIndex.sibling(nameIndex.row(),MODEL_VALUE_COLUMN);
	column+=nameWidth+1;
	group->insert(0,column,new ValueField(valueWidth,valueIndex,group,[](QString const& v){return v.right(8);}));
	const auto commentIndex=nameIndex.sibling(nameIndex.row(),MODEL_COMMENT_COLUMN);
	column+=valueWidth+1;
	group->insert(0,column,new FieldWidget(0,commentIndex,group));

	return group;
}

RegisterGroup* createExpandedEFL(RegisterViewModelBase::Model* model,QWidget* parent)
{
	const auto catIndex=findModelCategory(model,"General Status");
	if(!catIndex.isValid()) return nullptr;
	auto regNameIndex=findModelRegister(catIndex,"RFLAGS");
	if(!regNameIndex.isValid())
		regNameIndex=findModelRegister(catIndex,"EFLAGS");
	if(!regNameIndex.isValid()) return nullptr;
	auto* const group=new RegisterGroup(QObject::tr("Expanded EFL"), parent);
	static const std::unordered_map<char,QString> flagTooltips=
		{
		{'C',QObject::tr("Carry flag")+" (CF)"},
		{'P',QObject::tr("Parity flag")+" (PF)"},
		{'A',QObject::tr("Auxiliary carry flag")+" (AF)"},
		{'Z',QObject::tr("Zero flag")+" (ZF)"},
		{'S',QObject::tr("Sign flag")+" (SF)"},
		{'T',QObject::tr("Trap flag")+" (TF)"},
		{'D',QObject::tr("Direction flag")+" (DF)"},
		{'O',QObject::tr("Overflow flag")+" (OF)"}
		};
	for(int row=0,groupRow=0;row<model->rowCount(regNameIndex);++row)
	{
		const auto flagNameIndex=model->index(row,MODEL_NAME_COLUMN,regNameIndex);
		const auto flagValueIndex=model->index(row,MODEL_VALUE_COLUMN,regNameIndex);
		const auto flagName=model->data(flagNameIndex).toString().toUpper();
		if(flagName.length()!=2 || flagName[1]!='F') continue;
		static const int flagNameWidth=1;
		static const int valueWidth=1;
		const char name=flagName[0].toLatin1();
		switch(name)
		{
		case 'C':
		case 'P':
		case 'A':
		case 'Z':
		case 'S':
		case 'T':
		case 'D':
		case 'O':
		{
			const auto nameField=new FieldWidget(QChar(name),group);
			group->insert(groupRow,0,nameField);
			const auto valueField=new ValueField(valueWidth,flagValueIndex,group);
			group->insert(groupRow,flagNameWidth+1,valueField);
			++groupRow;

			const auto tooltipStr=flagTooltips.at(name);
			nameField->setToolTip(tooltipStr);
			valueField->setToolTip(tooltipStr);

			break;
		}
		default:
			continue;
		}
	}
	return group;
}

RegisterGroup* createFPUData(RegisterViewModelBase::Model* model,QWidget* parent)
{
	using RegisterViewModelBase::Model;

	const auto catIndex=findModelCategory(model,"FPU");
	if(!catIndex.isValid()) return nullptr;
	const auto tagsIndex=findModelRegister(catIndex,"FTR");
	if(!tagsIndex.isValid())
	{
		qWarning() << "Warning: failed to find FTR in the model, refusing to continue making FPUData group";
		return nullptr;
	}
	auto* const group=new RegisterGroup(QObject::tr("FPU Data Registers"),parent);
	static const int FPU_REG_COUNT=8;
	static const int nameWidth=3;
	static const int tagWidth=7;
	const auto fsrIndex=VALID_INDEX(findModelRegister(catIndex,"FSR"));
	const QPersistentModelIndex topIndex=VALID_INDEX(findModelRegister(fsrIndex,"TOP",MODEL_VALUE_COLUMN));
	for(int row=0;row<FPU_REG_COUNT;++row)
	{
		int column=0;
		const auto nameIndex=model->index(row,MODEL_NAME_COLUMN,catIndex);
		{
			const auto STiFormatter=[row,topIndex]()
			{
				const auto topByteArray=topIndex.data(Model::RawValueRole).toByteArray();
				if(topByteArray.isEmpty()) return QString("R%1").arg(row);
				const auto top=topByteArray[0];
				Q_ASSERT(top>=0); Q_ASSERT(top<8);
				const auto stI=(row+8-top) % 8;
				return QString("ST%1").arg(stI);
			};
			const auto field=new VolatileNameField(nameWidth,STiFormatter,group);
			QObject::connect(model,SIGNAL(dataChanged(QModelIndex const&,QModelIndex const&)),field,SLOT(update()));
			group->insert(row,column,field);
			column+=nameWidth+1;
		}
		const auto tagCommentIndex=VALID_INDEX(model->index(row,MODEL_COMMENT_COLUMN,tagsIndex));
		group->insert(row,column,new ValueField(tagWidth,tagCommentIndex,group,
												[](QString const&s){return s.toLower();}));
		column+=tagWidth+1;
		// Always show float-formatted value, not raw
		const auto regValueIndex=findModelRegister(nameIndex,"FLOAT",MODEL_VALUE_COLUMN);
		const int regValueWidth=regValueIndex.data(Model::FixedLengthRole).toInt();
		Q_ASSERT(regValueWidth>0);
		group->insert(row,column,new ValueField(regValueWidth,regValueIndex,group));
		column+=regValueWidth+1;
		const auto regCommentIndex=model->index(row,MODEL_COMMENT_COLUMN,catIndex);
		group->insert(row,column,new FieldWidget(0,regCommentIndex,group));
	}
	return group;
}

RegisterGroup* createFPUWords(RegisterViewModelBase::Model* model,QWidget* parent)
{
	const auto catIndex=findModelCategory(model,"FPU");
	if(!catIndex.isValid()) return nullptr;
	auto* const group=new RegisterGroup(QObject::tr("FPU Status&&Control Registers"),parent);
	group->appendNameValueComment(findModelRegister(catIndex,"FTR"),QObject::tr("FPU Tag Register"),false);
	const int fsrRow=1;
	const auto fsrIndex=findModelRegister(catIndex,"FSR");
	group->appendNameValueComment(fsrIndex,QObject::tr("FPU Status Register"),false);
	const int fcrRow=2;
	const auto fcrIndex=findModelRegister(catIndex,"FCR");
	group->appendNameValueComment(fcrIndex,QObject::tr("FPU Control Register"),false);

	const int wordNameWidth=3, wordValWidth=4;
	const int condPrecLabelColumn=wordNameWidth+1+wordValWidth+1+1;
	const int condPrecLabelWidth=4;
	group->insert(fsrRow,condPrecLabelColumn,new FieldWidget("Cond",group));
	group->insert(fcrRow,condPrecLabelColumn,new FieldWidget("Prec",group));
	const int condPrecValColumn=condPrecLabelColumn+condPrecLabelWidth+1;
	const int roundModeWidth=4, precModeWidth=2;
	const int roundModeColumn=condPrecValColumn;
	const int precModeColumn=roundModeColumn+roundModeWidth+1;
	// This must be inserted before precision&rounding value fields, since they overlap this label
	group->insert(fcrRow,precModeColumn-1,new FieldWidget(",",group));
	for(int condN=3;condN>=0;--condN)
	{
		const auto name=QString("C%1").arg(condN);
		const auto condNNameIndex=VALID_INDEX(findModelRegister(fsrIndex,name));
		const auto condNIndex=VALID_INDEX(condNNameIndex.sibling(condNNameIndex.row(),MODEL_VALUE_COLUMN));
		const int column=condPrecValColumn+2*(3-condN);
		const auto nameField=new FieldWidget(QString("%1").arg(condN),group);
		group->insert(fsrRow-1,column,nameField);
		const auto valueField=new ValueField(1,condNIndex,group);
		group->insert(fsrRow, column,valueField);

		nameField->setToolTip(name);
		valueField->setToolTip(name);
	}
	addRoundingMode(group,findModelRegister(fcrIndex,"RC",MODEL_VALUE_COLUMN),fcrRow,roundModeColumn);
	addPrecisionMode(group,findModelRegister(fcrIndex,"PC",MODEL_VALUE_COLUMN),fcrRow,precModeColumn);
	const int errMaskColumn=precModeColumn+precModeWidth+2;
	const int errLabelWidth=3;
	group->insert(fsrRow,errMaskColumn,new FieldWidget("Err",group));
	group->insert(fcrRow,errMaskColumn,new FieldWidget("Mask",group));
	const int ESColumn=errMaskColumn+errLabelWidth+1;
	const int SFColumn=ESColumn+2;
	const auto ESNameField=new FieldWidget("E",group);
	group->insert(fsrRow-1,ESColumn,ESNameField);
	const auto SFNameField=new FieldWidget("S",group);
	group->insert(fsrRow-1,SFColumn,SFNameField);
	const auto ESValueField=new ValueField(1,findModelRegister(fsrIndex,"ES",MODEL_VALUE_COLUMN),group);
	group->insert(fsrRow,ESColumn,ESValueField);
	const auto SFValueField=new ValueField(1,findModelRegister(fsrIndex,"SF",MODEL_VALUE_COLUMN),group);
	group->insert(fsrRow,SFColumn,SFValueField);

	{
		const auto ESTooltip=QObject::tr("Error Summary Status")+" (ES)";
		ESNameField->setToolTip(ESTooltip);
		ESValueField->setToolTip(ESTooltip);
	}
	{
		const auto SFTooltip=QObject::tr("Stack Fault")+" (SF)";
		SFNameField->setToolTip(SFTooltip);
		SFValueField->setToolTip(SFTooltip);
	}

	const int PEPMColumn=SFColumn+2;
	addPUOZDI(group,fsrIndex,fcrIndex,fsrRow-1,PEPMColumn);
	const int PUOZDIWidth=6*2-1;
	group->insert(fsrRow,PEPMColumn+PUOZDIWidth+1,new FieldWidget(0,getCommentIndex(fsrIndex),group));

	return group;
}

// Checks that FOP is in not in compatibility mode, i.e. is updated only on unmasked exception
// This function would return false for e.g. Pentium III or Atom, but returns true since Pentium 4.
// This can be made return false for such CPUs by setting bit 2 in IA32_MISC_ENABLE MSR.
bool FOPIsIncompatible()
{
	char fenv[28];
	asm volatile("fldz\n"
				 "fstp %%st(0)\n"
				 "fstenv %0\n"
				 :"=m"(fenv)::"%st");
	std::uint16_t fop;
	std::memcpy(&fop,fenv+18,sizeof fop);
	return fop==0;
}

RegisterGroup* createFPULastOp(RegisterViewModelBase::Model* model,QWidget* parent)
{
	using RegisterViewModelBase::Model;

	const auto catIndex=findModelCategory(model,"FPU");
	if(!catIndex.isValid()) return nullptr;
	const auto FIPIndex=findModelRegister(catIndex,"FIP",MODEL_VALUE_COLUMN);
	if(!FIPIndex.isValid()) return nullptr;
	const auto FDPIndex=findModelRegister(catIndex,"FDP",MODEL_VALUE_COLUMN);
	if(!FDPIndex.isValid()) return nullptr;

	auto* const group=new RegisterGroup(QObject::tr("FPU Last Operation Registers"),parent);
	enum {lastInsnRow, lastDataRow, lastOpcodeRow};
	const QString lastInsnLabel="Last insn";
	const QString lastDataLabel="Last data";
	const QString lastOpcodeLabel="Last opcode";
	const auto lastInsnLabelField=new FieldWidget(lastInsnLabel,group);
	group->insert(lastInsnRow,0,lastInsnLabelField);
	const auto lastDataLabelField=new FieldWidget(lastDataLabel,group);
	group->insert(lastDataRow,0,lastDataLabelField);
	const auto lastOpcodeLabelField=new FieldWidget(lastOpcodeLabel,group);
	group->insert(lastOpcodeRow,0,lastOpcodeLabelField);

	lastInsnLabelField->setToolTip(QObject::tr("Last FPU instruction address"));
	lastDataLabelField->setToolTip(QObject::tr("Last FPU memory operand address"));

	// FIS & FDS are not maintained in 64-bit mode; Linux64 always saves state from
	// 64-bit mode, losing the values for 32-bit apps even if the CPU doesn't deprecate them
	// We'll show zero offsets in 32 bit mode for consistency with 32-bit kernels
	// In 64-bit mode, since segments are not maintained, we'll just show offsets
	const auto FIPwidth=FDPIndex.data(Model::FixedLengthRole).toInt();
	const auto segWidth = FIPwidth==8/*8chars=>32bit*/ ? 4 : 0;
	const auto segColumn=lastInsnLabel.length()+1;
	if(segWidth)
	{
		// these two must be inserted first, because seg & offset value fields overlap these labels
		group->insert(lastInsnRow,segColumn+segWidth,new FieldWidget(":",group));
		group->insert(lastDataRow,segColumn+segWidth,new FieldWidget(":",group));

		const auto FISField=new ValueField(segWidth,findModelRegister(catIndex,"FIS",MODEL_VALUE_COLUMN),group);
		group->insert(lastInsnRow,segColumn,FISField);
		const auto FDSField=new ValueField(segWidth,findModelRegister(catIndex,"FDS",MODEL_VALUE_COLUMN),group);
		group->insert(lastDataRow,segColumn,FDSField);

		FISField->setToolTip(QObject::tr("Last FPU instruction selector"));
		FDSField->setToolTip(QObject::tr("Last FPU memory operand selector"));
	}
	const auto offsetWidth=FIPIndex.data(Model::FixedLengthRole).toInt();
	Q_ASSERT(offsetWidth>0);
	const auto offsetColumn=segColumn+segWidth+(segWidth?1:0);
	const auto FIPValueField=new ValueField(offsetWidth,FIPIndex,group);
	group->insert(lastInsnRow,offsetColumn,FIPValueField);
	const auto FDPValueField=new ValueField(offsetWidth,FDPIndex,group);
	group->insert(lastDataRow,offsetColumn,FDPValueField);

	FIPValueField->setToolTip(QObject::tr("Last FPU instruction offset"));
	FDPValueField->setToolTip(QObject::tr("Last FPU memory operand offset"));

	QPersistentModelIndex const FOPIndex=findModelRegister(catIndex,"FOP",MODEL_VALUE_COLUMN);
	QPersistentModelIndex const FSRIndex=findModelRegister(catIndex,"FSR",MODEL_VALUE_COLUMN);
	QPersistentModelIndex const FCRIndex=findModelRegister(catIndex,"FCR",MODEL_VALUE_COLUMN);
	bool fopRarelyUpdated=FOPIsIncompatible();
	const auto FOPFormatter=[FOPIndex,FSRIndex,FCRIndex,FIPIndex,fopRarelyUpdated](QString const& str)
	{
		if(str.isEmpty() || str[0]=='?') return str;

		const auto rawFCR=FCRIndex.data(Model::RawValueRole).toByteArray();
		Q_ASSERT(rawFCR.size()<=long(sizeof(edb::value16)));
		if(rawFCR.isEmpty()) return str;
		edb::value16 fcr(0);
		std::memcpy(&fcr,rawFCR.constData(),rawFCR.size());

		const auto rawFSR=FSRIndex.data(Model::RawValueRole).toByteArray();
		Q_ASSERT(rawFSR.size()<=long(sizeof(edb::value16)));
		if(rawFSR.isEmpty()) return str;
		edb::value16 fsr(0);
		std::memcpy(&fsr,rawFSR.constData(),rawFSR.size());

		const auto rawFOP=FOPIndex.data(Model::RawValueRole).toByteArray();
		edb::value16 fop(0);
		Q_ASSERT(rawFOP.size()<=long(sizeof(edb::value16)));
		if(rawFOP.isEmpty()) return str;
		if(rawFOP.size()!=sizeof(edb::value16))
			return QString("????");
		std::memcpy(&fop,rawFOP.constData(),rawFOP.size());

		const auto rawFIP=FIPIndex.data(Model::RawValueRole).toByteArray();
		if(rawFIP.isEmpty()) return str;
		edb::address_t fip(0);
		Q_ASSERT(rawFIP.size()<=long(sizeof fip));
		std::memcpy(&fip,rawFIP.constData(),rawFIP.size());	

		const auto excMask=fcr&0x3f;
		const auto excActive=fsr&0x3f;
		const auto excActiveUnmasked=excActive&~excMask;
		if(fop==0 && ((fopRarelyUpdated && !excActiveUnmasked) || fip==0))
			return QString("00 00");
		return edb::value8(0xd8+rawFOP[1]).toHexString()+
				' '+edb::value8(rawFOP[0]).toHexString();
	};
	const auto FOPValueField=new ValueField(5,FOPIndex,group,FOPFormatter);
	group->insert(lastOpcodeRow,lastOpcodeLabel.length()+1,FOPValueField);

	static const auto FOPTooltip=QObject::tr("Last FPU opcode");
	lastOpcodeLabelField->setToolTip(FOPTooltip);
	FOPValueField->setToolTip(FOPTooltip);

	return group;
}

RegisterGroup* createDebugGroup(RegisterViewModelBase::Model* model,QWidget* parent)
{
	using RegisterViewModelBase::Model;

	const auto catIndex=findModelCategory(model,"Debug");
	if(!catIndex.isValid()) return nullptr;

	auto* const group=new RegisterGroup(QObject::tr("Debug Registers"),parent);

	const auto dr6Index=VALID_INDEX(findModelRegister(catIndex,"DR6"));
	const auto dr7Index=VALID_INDEX(findModelRegister(catIndex,"DR7"));
	const auto nameWidth=3;
	const auto valueWidth=getValueIndex(dr6Index).data(Model::FixedLengthRole).toInt();
	Q_ASSERT(valueWidth>0);
	int row=0;
	const auto bitsSpacing=1;
	const auto BTooltip=QObject::tr("Breakpoint Condition Detected");
	const auto LTooltip=QObject::tr("Local Breakpoint Enable");
	const auto GTooltip=QObject::tr("Global Breakpoint Enable");
	const auto typeTooltip=QObject::tr("Breakpoint condition");
	const auto lenTooltip=QObject::tr("Data breakpoint length");
	const auto lenDecodedStr=QObject::tr(" (bytes count from %1)");
	{
		int column=nameWidth+1+valueWidth+2;
		const auto BLabelField=new FieldWidget("B",group);
		BLabelField->setToolTip(BTooltip+" (B0..B3)");
		group->insert(row,column,BLabelField);
		column+=bitsSpacing+1;
		const auto LLabelField=new FieldWidget("L",group);
		LLabelField->setToolTip(LTooltip+" (L0..L3)");
		group->insert(row,column,LLabelField);
		column+=bitsSpacing+1;
		const auto GLabelField=new FieldWidget("G",group);
		GLabelField->setToolTip(GTooltip+" (G0..G3)");
		group->insert(row,column,GLabelField);
		column+=bitsSpacing+1;
		const auto typeLabelField=new FieldWidget("Type",group);
		typeLabelField->setToolTip(typeTooltip+" (R/W0..R/W3)");
		group->insert(row,column,typeLabelField);
		column+=bitsSpacing+4;
		const auto lenLabelField=new FieldWidget("Len",group);
		lenLabelField->setToolTip(lenTooltip+lenDecodedStr.arg("LEN0..LEN3"));
		group->insert(row,column,lenLabelField);
		column+=bitsSpacing+3;

		++row;
	}
	for(int drI=0;drI<4;++drI,++row)
	{
		const auto name=QString("DR%1").arg(drI);
		const auto DRiValueIndex=VALID_INDEX(findModelRegister(catIndex,name,MODEL_VALUE_COLUMN));
		int column=0;
		group->insert(row,column,new FieldWidget(name,group));
		column+=nameWidth+1;
		group->insert(row,column,new ValueField(valueWidth,DRiValueIndex,group));
		column+=valueWidth+2;
		{
			const auto BiName=QString("B%1").arg(drI);
			const auto BiIndex=VALID_INDEX(findModelRegister(dr6Index,BiName,MODEL_VALUE_COLUMN));
			const auto BiValueField=new ValueField(1,BiIndex,group);
			BiValueField->setToolTip(BTooltip+" ("+BiName+")");
			group->insert(row,column,BiValueField);
			column+=bitsSpacing+1;
		}
		{
			const auto LiName=QString("L%1").arg(drI);
			const auto LiIndex=VALID_INDEX(findModelRegister(dr7Index,LiName,MODEL_VALUE_COLUMN));
			const auto LiValueField=new ValueField(1,LiIndex,group);
			LiValueField->setToolTip(LTooltip+" ("+LiName+")");
			group->insert(row,column,LiValueField);
			column+=bitsSpacing+1;
		}
		{
			const auto GiName=QString("G%1").arg(drI);
			const auto GiIndex=VALID_INDEX(findModelRegister(dr7Index,GiName,MODEL_VALUE_COLUMN));
			const auto GiValueField=new ValueField(1,GiIndex,group);
			GiValueField->setToolTip(GTooltip+" ("+GiName+")");
			group->insert(row,column,GiValueField);
			column+=bitsSpacing+1;
		}
		{
			const auto RWiName=QString("R/W%1").arg(drI);
			const QPersistentModelIndex RWiIndex=VALID_INDEX(findModelRegister(dr7Index,RWiName,MODEL_VALUE_COLUMN));
			const auto width=5;
			const auto RWiValueField=new ValueField(width,RWiIndex,group,[RWiIndex](QString const& str)->QString
						{
							if(str.isEmpty() || str[0]=='?') return "??";
							Q_ASSERT(str.size()==1);
							switch(str[0].toLatin1())
							{
							case '0': return "EXEC";
							case '1': return "WRITE";
							case '2': return " IO";
							case '3': return " R/W";
							default: return "???";
							}
						});
			RWiValueField->setToolTip(typeTooltip+" ("+RWiName+")");
			group->insert(row,column,RWiValueField);
			column+=bitsSpacing+width;
		}
		{
			const auto LENiName=QString("LEN%1").arg(drI);
			const QPersistentModelIndex LENiIndex=VALID_INDEX(findModelRegister(dr7Index,LENiName,MODEL_VALUE_COLUMN));
			const auto LENiValueField=new ValueField(1,LENiIndex,group,[LENiIndex](QString const& str)->QString
						{
							if(str.isEmpty() || str[0]=='?') return "??";
							Q_ASSERT(str.size()==1);
							switch(str[0].toLatin1())
							{
							case '0': return "1";
							case '1': return "2";
							case '2': return "8";
							case '3': return "4";
							default: return "???";
							}
						});
			LENiValueField->setToolTip(lenTooltip+lenDecodedStr.arg(LENiName));
			group->insert(row,column,LENiValueField);
		}
	}
	{
		int column=0;
		group->insert(row,column,new FieldWidget("DR6",group));
		column+=nameWidth+1;
		group->insert(row,column,new ValueField(valueWidth,getValueIndex(dr6Index),group));
		column+=valueWidth+2;
		const QString bsName="BS";
		const auto bsWidth=bsName.length();
		const auto BSNameField=new FieldWidget(bsName,group);
		const auto BSTooltip=QObject::tr("Single Step")+" (BS)";
		BSNameField->setToolTip(BSTooltip);
		group->insert(row,column,BSNameField);
		column+=bsWidth+1;
		const auto bsIndex=findModelRegister(dr6Index,bsName,MODEL_VALUE_COLUMN);
		const auto BSValueField=new ValueField(1,bsIndex,group);
		BSValueField->setToolTip(BSTooltip);
		group->insert(row,column,BSValueField);

		++row;
	}
	{
		int column=0;
		group->insert(row,column,new FieldWidget("DR7",group));
		column+=nameWidth+1;
		group->insert(row,column,new ValueField(valueWidth,getValueIndex(dr7Index),group));
		column+=valueWidth+2;
		{
			const QString leName="LE";
			const auto leWidth=leName.length();
			const auto LENameField=new FieldWidget(leName,group);
			const auto LETooltip=QObject::tr("Local Exact Breakpoint Enable");
			LENameField->setToolTip(LETooltip);
			group->insert(row,column,LENameField);
			column+=leWidth+1;
			const auto leIndex=findModelRegister(dr7Index,leName,MODEL_VALUE_COLUMN);
			const auto leValueWidth=1;
			const auto LEValueField=new ValueField(leValueWidth,leIndex,group);
			LEValueField->setToolTip(LETooltip);
			group->insert(row,column,LEValueField);
			column+=leValueWidth+1;
		}
		{
			const QString geName="GE";
			const auto geWidth=geName.length();
			const auto GENameField=new FieldWidget(geName,group);
			const auto GETooltip=QObject::tr("Global Exact Breakpoint Enable");
			GENameField->setToolTip(GETooltip);
			group->insert(row,column,GENameField);
			column+=geWidth+1;
			const auto geIndex=findModelRegister(dr7Index,geName,MODEL_VALUE_COLUMN);
			const auto geValueWidth=1;
			const auto GEValueField=new ValueField(geValueWidth,geIndex,group);
			GEValueField->setToolTip(GETooltip);
			group->insert(row,column,GEValueField);
			column+=geValueWidth+1;
		}
	}

	return group;
}

RegisterGroup* createMXCSR(RegisterViewModelBase::Model* model,QWidget* parent)
{
	using namespace RegisterViewModelBase;

	const auto catIndex=findModelCategory(model,"SSE");
	if(!catIndex.isValid()) return nullptr;
	auto* const group=new RegisterGroup("MXCSR",parent);
	const QString mxcsrName="MXCSR";
	int column=0;
	const int mxcsrRow=1, fzRow=mxcsrRow,dazRow=mxcsrRow,excRow=mxcsrRow;
	const int rndRow=fzRow+1, maskRow=rndRow;
	group->insert(mxcsrRow,column,new FieldWidget(mxcsrName,group));
	column+=mxcsrName.length()+1;
	const auto mxcsrIndex=findModelRegister(catIndex,"MXCSR",MODEL_VALUE_COLUMN);
	const auto mxcsrValueWidth=mxcsrIndex.data(Model::FixedLengthRole).toInt();
	Q_ASSERT(mxcsrValueWidth>0);
	group->insert(mxcsrRow,column,new ValueField(mxcsrValueWidth,mxcsrIndex,group));
	column+=mxcsrValueWidth+2;
	// XXX: Sacrificing understandability of DAZ->DZ to align PUOZDI with FPU's.
	// Also FZ value is one char away from DAZ name, which is also no good.
	// Maybe following OllyDbg example here isn't a good idea.
	const QString fzName="FZ", dazName="DZ";
	const auto fzColumn=column;
	const auto fzNameField=new FieldWidget(fzName,group);
	group->insert(fzRow,fzColumn,fzNameField);
	column+=fzName.length()+1;
	const auto fzIndex=findModelRegister(mxcsrIndex,"FZ",MODEL_VALUE_COLUMN);
	const auto fzValueWidth=1;
	const auto fzValueField=new ValueField(fzValueWidth,fzIndex,group);
	group->insert(fzRow,column,fzValueField);
	column+=fzValueWidth+1;
	const auto dazNameField=new FieldWidget(dazName,group);
	group->insert(dazRow,column,dazNameField);
	column+=dazName.length()+1;
	const auto dazIndex=findModelRegister(mxcsrIndex,"DAZ",MODEL_VALUE_COLUMN);
	const auto dazValueWidth=1;
	const auto dazValueField=new ValueField(dazValueWidth,dazIndex,group);
	group->insert(dazRow,column,dazValueField);
	column+=dazValueWidth+2;
	const QString excName="Err";
	group->insert(excRow,column,new FieldWidget(excName,group));
	const QString maskName="Mask";
	group->insert(maskRow,column,new FieldWidget(maskName,group));
	column+=maskName.length()+1;
	addPUOZDI(group,mxcsrIndex,mxcsrIndex,excRow-1,column);
	const auto rndNameColumn=fzColumn;
	const QString rndName="Rnd";
	group->insert(rndRow,rndNameColumn,new FieldWidget(rndName,group));
	const auto rndColumn=rndNameColumn+rndName.length()+1;
	addRoundingMode(group,findModelRegister(mxcsrIndex,"RC",MODEL_VALUE_COLUMN),rndRow,rndColumn);

	{
		const auto fzTooltip=QObject::tr("Flush Denormals To Zero")+" (FTZ)";
		fzNameField->setToolTip(fzTooltip);
		fzValueField->setToolTip(fzTooltip);
	}
	{
		const auto dazTooltip=QObject::tr("Denormals Are Zeros")+" (DAZ)";
		dazNameField->setToolTip(dazTooltip);
		dazValueField->setToolTip(dazTooltip);
	}

	return group;
}

RegisterGroup* createSIMDGroup(RegisterViewModelBase::Model* model,QWidget* parent,QString const& catName,QString const& regNamePrefix)
{
	const auto catIndex=findModelCategory(model,catName);
	if(!catIndex.isValid()) return nullptr;
	auto* const group=new RegisterGroup(catName,parent);
	for(int row=0;row<model->rowCount(catIndex);++row)
	{
		const auto nameIndex=VALID_INDEX(model->index(row,MODEL_NAME_COLUMN,catIndex));
		const auto name=regNamePrefix+QString("%1").arg(row);
		if(!VALID_VARIANT(nameIndex.data()).toString().toUpper().startsWith(regNamePrefix))
		{
			if(row==0) return nullptr; // don't want empty groups
			break;
		}
		group->insert(row,0,new FieldWidget(name,group));
		new SIMDValueManager(row,nameIndex,group);
	}
	// This signal must be handled by group _after_ all `SIMDValueManager`s handle their connection to this signal
	QObject::connect(model,SIGNAL(SIMDDisplayFormatChanged()),group,SLOT(adjustWidth()),Qt::QueuedConnection);
	return group;
}

RegisterGroup* ODBRegView::makeGroup(RegisterGroupType type)
{
	if(!model_->rowCount()) return nullptr;
	std::vector<QModelIndex> nameValCommentIndices;
	using RegisterViewModelBase::Model;
	QString groupName;
	switch(type)
	{
	case RegisterGroupType::EFL: return createEFL(model_,widget());
	case RegisterGroupType::ExpandedEFL: return createExpandedEFL(model_,widget());
	case RegisterGroupType::FPUData: return createFPUData(model_,widget());
	case RegisterGroupType::FPUWords: return createFPUWords(model_,widget());
	case RegisterGroupType::FPULastOp: return createFPULastOp(model_,widget());
	case RegisterGroupType::Debug: return createDebugGroup(model_,widget());
	case RegisterGroupType::MXCSR: return createMXCSR(model_,widget());
	case RegisterGroupType::MMX: return createSIMDGroup(model_,widget(),"MMX","MM");
	case RegisterGroupType::SSEData: return createSIMDGroup(model_,widget(),"SSE","XMM");
	case RegisterGroupType::AVXData: return createSIMDGroup(model_,widget(),"AVX","YMM");
	case RegisterGroupType::GPR:
	{
		groupName=tr("GPRs");
		const auto catIndex=findModelCategory(model_,"General Purpose");
		if(!catIndex.isValid()) break;
		for(int row=0;row<model_->rowCount(catIndex);++row)
			nameValCommentIndices.emplace_back(model_->index(row,MODEL_NAME_COLUMN,catIndex));
		break;
	}
	case RegisterGroupType::Segment:
	{
		groupName=tr("Segment Registers");
		const auto catIndex=findModelCategory(model_,"Segment");
		if(!catIndex.isValid()) break;
		for(int row=0;row<model_->rowCount(catIndex);++row)
			nameValCommentIndices.emplace_back(model_->index(row,MODEL_NAME_COLUMN,catIndex));
		break;
	}
	case RegisterGroupType::rIP:
	{
		groupName=tr("Instruction Pointer");
		const auto catIndex=findModelCategory(model_,"General Status");
		if(!catIndex.isValid()) break;
		nameValCommentIndices.emplace_back(findModelRegister(catIndex,"RIP"));
		nameValCommentIndices.emplace_back(findModelRegister(catIndex,"EIP"));
		break;
	}
	default:
		qWarning() << "Warning: unexpected register group type requested in" << Q_FUNC_INFO;
		return nullptr;
	}
	nameValCommentIndices.erase(std::remove_if(nameValCommentIndices.begin(),
											   nameValCommentIndices.end(),
											   [](QModelIndex const& index){ return !index.isValid(); })
								,nameValCommentIndices.end());
	if(nameValCommentIndices.empty())
	{
		qWarning() << "Warning: failed to get any useful register indices for regGroupType" << static_cast<long>(type);
		return nullptr;
	}
	auto* const group=new RegisterGroup(groupName,widget());
	for(const auto& index : nameValCommentIndices)
		group->appendNameValueComment(index);
	return group;
}

void ODBRegView::modelReset()
{
	widget()->hide(); // prevent flicker while groups are added to/removed from the layout
	// not all groups may be in the layout, so delete them individually
	for(auto* const group : groups)
		if(group)
			group->deleteLater();
	groups.clear();

	auto* const layout=static_cast<QVBoxLayout*>(widget()->layout());

	// layout contains not only groups, so delete all items too
	while(auto* const item=layout->takeAt(0))
		delete item;

	auto* const flagsAndSegments=new QHBoxLayout();
	// (3/2+1/2)-letter — Total of 2-letter spacing. Fourth half-letter is from flag values extension.
	// Segment extensions at LHS of the widget don't influence minimumSize request, so no need to take
	// them into account.
	flagsAndSegments->setSpacing(letterSize(this->font()).width()*3/2);
	flagsAndSegments->setContentsMargins(QMargins());
	flagsAndSegments->setAlignment(Qt::AlignLeft);

	bool flagsAndSegsInserted=false;
	for(int groupType_=0;groupType_<RegisterGroupType::NUM_GROUPS;++groupType_)
	{
		const auto groupType=RegisterGroupType{groupType_};
		if(util::contains(visibleGroupTypes,groupType))
		{
			auto*const group=makeGroup(groupType);
			groups.push_back(group);
			if(!group) continue;
			if(groupType==RegisterGroupType::Segment || groupType==RegisterGroupType::ExpandedEFL)
			{
				flagsAndSegments->addWidget(group);
				if(!flagsAndSegsInserted)
				{
					layout->addLayout(flagsAndSegments);
					flagsAndSegsInserted=true;
				}
			}
			else layout->addWidget(group);
		}
		else groups.push_back(nullptr);
	}
	widget()->show();
}

void ODBRegView::modelUpdated()
{
	for(auto* const field : fields())
		field->update();
	for(auto* const group : groups)
		if(group) group->adjustWidth();
}

QList<FieldWidget*> ODBRegView::fields() const
{
	QList<FieldWidget*> allFields;
	for(auto* const group : groups)
		if(group)
			allFields.append(group->fields());
	return allFields;
}

QList<ValueField*> ODBRegView::valueFields() const
{
	QList<ValueField*> allValues;
	for(auto* const group : groups)
		if(group)
			allValues.append(group->valueFields());
	return allValues;

}

void ODBRegView::updateFieldsPalette()
{
	for(auto* const field : valueFields())
		field->updatePalette();
}

ValueField* ODBRegView::selectedField() const
{
	for(auto* const field : valueFields())
		if(field->isSelected()) return field;
	return nullptr;
}

SIMDValueManager::SIMDValueManager(int lineInGroup, QModelIndex const& nameIndex, RegisterGroup* parent)
	: QObject(parent),
	  regIndex(nameIndex),
	  lineInGroup(lineInGroup),
	  intMode(NumberDisplayMode::Hex)
{
	setupMenu();

	Q_ASSERT(nameIndex.isValid());
	connect(nameIndex.model(),SIGNAL(SIMDDisplayFormatChanged()),this,SLOT(displayFormatChanged()));
	displayFormatChanged();
}

void SIMDValueManager::fillGroupMenu()
{
	const auto group=this->group();
	group->menuItems.push_back(newActionSeparator(this));
	group->menuItems.push_back(menuItems[VIEW_AS_BYTES]);
	group->menuItems.push_back(menuItems[VIEW_AS_WORDS]);
	group->menuItems.push_back(menuItems[VIEW_AS_DWORDS]);
	group->menuItems.push_back(menuItems[VIEW_AS_QWORDS]);
	group->menuItems.push_back(newActionSeparator(this));
	group->menuItems.push_back(menuItems[VIEW_AS_FLOAT32]);
	group->menuItems.push_back(menuItems[VIEW_AS_FLOAT64]);
	group->menuItems.push_back(newActionSeparator(this));
	group->menuItems.push_back(menuItems[VIEW_INT_AS_HEX]);
	group->menuItems.push_back(menuItems[VIEW_INT_AS_SIGNED]);
	group->menuItems.push_back(menuItems[VIEW_INT_AS_UNSIGNED]);
}

auto SIMDValueManager::model() const -> Model*
{
	const auto model=static_cast<const Model*>(regIndex.model());
	// The model is not supposed to have been created as const object,
	// and our manipulations won't invalidate the index.
	// Thus cast the const away.
	return const_cast<Model*>(model);
}

void SIMDValueManager::showAsInt(int const size_)
{
	const auto size=static_cast<Model::ElementSize>(size_);
	model()->setChosenSIMDSize(regIndex.parent(),size);
	model()->setChosenSIMDFormat(regIndex.parent(),intMode);
}

void SIMDValueManager::showAsFloat(int const size)
{
	model()->setChosenSIMDFormat(regIndex.parent(),NumberDisplayMode::Float);
	switch(size)
	{
	case sizeof(edb::value32):
		model()->setChosenSIMDSize(regIndex.parent(),Model::ElementSize::DWORD);
		break;
	case sizeof(edb::value64):
		model()->setChosenSIMDSize(regIndex.parent(),Model::ElementSize::QWORD);
		break;
	default: EDB_PRINT_AND_DIE("Unexpected size: ",size);
	}
}

void SIMDValueManager::setIntFormat(int format_)
{
	const auto format=static_cast<NumberDisplayMode>(format_);
	model()->setChosenSIMDFormat(regIndex.parent(),format);
}

void SIMDValueManager::setupMenu()
{
	const auto group=this->group();
	const auto validFormats=VALID_VARIANT(regIndex.parent()
												  .data(Model::ValidSIMDFormatsRole))
												  .value<std::vector<NumberDisplayMode>>();
	// Setup menu if we're the first value field creator
	if(group->valueFields().isEmpty())
	{
		const auto intSizeMapper=new QSignalMapper(this);
		connect(intSizeMapper,SIGNAL(mapped(int)),this,SLOT(showAsInt(int)));
		menuItems.push_back(newAction(tr("View %1 as bytes").arg(group->name),
							group,intSizeMapper,Model::ElementSize::BYTE));
		menuItems.push_back(newAction(tr("View %1 as words").arg(group->name),
							group,intSizeMapper,Model::ElementSize::WORD));
		menuItems.push_back(newAction(tr("View %1 as doublewords").arg(group->name),
							group,intSizeMapper,Model::ElementSize::DWORD));
		menuItems.push_back(newAction(tr("View %1 as quadwords").arg(group->name),
							group,intSizeMapper,Model::ElementSize::QWORD));

		if(util::contains(validFormats,NumberDisplayMode::Float))
		{
			const auto floatMapper=new QSignalMapper(this);
			connect(floatMapper,SIGNAL(mapped(int)),this,SLOT(showAsFloat(int)));
			menuItems.push_back(newAction(tr("View %1 as 32-bit floats").arg(group->name),
								group,floatMapper,Model::ElementSize::DWORD));
			menuItems.push_back(newAction(tr("View %1 as 64-bit floats").arg(group->name),
								group,floatMapper,Model::ElementSize::QWORD));
		}
		else
		{
			// create empty elements to leave further items with correct indices
			menuItems.push_back(newActionSeparator(this));
			menuItems.push_back(newActionSeparator(this));
		}

		const auto intMapper=new QSignalMapper(this);
		connect(intMapper,SIGNAL(mapped(int)),this,SLOT(setIntFormat(int)));
		menuItems.push_back(newAction(tr("View %1 integers as hex").arg(group->name),
							group,intMapper,static_cast<int>(NumberDisplayMode::Hex)));
		menuItems.push_back(newAction(tr("View %1 integers as signed").arg(group->name),
							group,intMapper,static_cast<int>(NumberDisplayMode::Signed)));
		menuItems.push_back(newAction(tr("View %1 integers as unsigned").arg(group->name),
							group,intMapper,static_cast<int>(NumberDisplayMode::Unsigned)));

		fillGroupMenu();
	}
}

void SIMDValueManager::updateMenu()
{
	if(menuItems.isEmpty()) return;
	for(auto item : menuItems)
		item->setVisible(true);

	using RegisterViewModelBase::Model;
	switch(currentSize())
	{
	case Model::ElementSize::BYTE:  menuItems[VIEW_AS_BYTES ]->setVisible(false); break;
	case Model::ElementSize::WORD:  menuItems[VIEW_AS_WORDS ]->setVisible(false); break;
	case Model::ElementSize::DWORD:
		menuItems[VIEW_AS_DWORDS]->setVisible(false);
		if(currentFormat()==NumberDisplayMode::Float)
			menuItems[VIEW_AS_FLOAT32]->setVisible(false);
		break;
	case Model::ElementSize::QWORD:
		menuItems[VIEW_AS_QWORDS]->setVisible(false);
		if(currentFormat()==NumberDisplayMode::Float)
			menuItems[VIEW_AS_FLOAT64]->setVisible(false);
		break;
	default: EDB_PRINT_AND_DIE("Unexpected current size: ",currentSize());
	}
	switch(currentFormat())
	{
	case NumberDisplayMode::Float:
		menuItems[  VIEW_INT_AS_HEX   ]->setVisible(false);
		menuItems[ VIEW_INT_AS_SIGNED ]->setVisible(false);
		menuItems[VIEW_INT_AS_UNSIGNED]->setVisible(false);
		break;
	case NumberDisplayMode::Hex:      menuItems[  VIEW_INT_AS_HEX   ]->setVisible(false); break;
	case NumberDisplayMode::Signed:   menuItems[ VIEW_INT_AS_SIGNED ]->setVisible(false); break;
	case NumberDisplayMode::Unsigned: menuItems[VIEW_INT_AS_UNSIGNED]->setVisible(false); break;
	}
}

RegisterGroup* SIMDValueManager::group() const
{
	Q_ASSERT(dynamic_cast<RegisterGroup*>(parent()));
	return static_cast<RegisterGroup*>(parent());
}

void SIMDValueManager::displayFormatChanged()
{
	const auto newFormat=currentFormat();
	if(newFormat!=NumberDisplayMode::Float)
		intMode=newFormat;

	for(auto* const elem : elements)
		elem->deleteLater();
	elements.clear();

	using RegisterViewModelBase::Model;
	const auto model=regIndex.model();

	const int sizeRow=VALID_VARIANT(regIndex.parent().data(Model::ChosenSIMDSizeRowRole)).toInt();
	QModelIndex sizeIndex=model->index(sizeRow,MODEL_NAME_COLUMN,regIndex);
	const auto elemCount=model->rowCount(sizeIndex);

	const auto regNameWidth=VALID_VARIANT(regIndex.data(Model::FixedLengthRole)).toInt();
	int column=regNameWidth+1;
	const auto elemWidth=VALID_VARIANT(model->index(0,MODEL_VALUE_COLUMN,sizeIndex).data(Model::FixedLengthRole)).toInt();
	for(int elemN=elemCount-1;elemN>=0;--elemN)
	{
		const auto elemIndex=model->index(elemN,MODEL_VALUE_COLUMN,sizeIndex);
		const auto field=new ValueField(elemWidth,elemIndex,group());
		elements.push_back(field);
		field->setAlignment(Qt::AlignRight);
		group()->insert(lineInGroup,column,field);
		column+=elemWidth+1;
	}

	updateMenu();
}

RegisterViewModelBase::Model::ElementSize SIMDValueManager::currentSize() const
{
	using RegisterViewModelBase::Model;
	const int size=VALID_VARIANT(regIndex.parent().data(Model::ChosenSIMDSizeRole)).toInt();
	return static_cast<Model::ElementSize>(size);
}

NumberDisplayMode SIMDValueManager::currentFormat() const
{
	using RegisterViewModelBase::Model;
	const int size=VALID_VARIANT(regIndex.parent().data(Model::ChosenSIMDFormatRole)).toInt();
	return static_cast<NumberDisplayMode>(size);
}

void ODBRegView::keyPressEvent(QKeyEvent* event)
{
	auto* const selected=selectedField();
	switch(event->key())
	{
	case Qt::Key_Up:
		if(selected && selected->up())
		{
			selected->up()->select();
			return;
		}
		break;
	case Qt::Key_Down:
		if(selected && selected->down())
		{
			selected->down()->select();
			return;
		}
		break;
	case Qt::Key_Left:
		if(selected && selected->left())
		{
			selected->left()->select();
			return;
		}
		break;
	case Qt::Key_Right:
		if(selected && selected->right())
		{
			selected->right()->select();
			return;
		}
		break;
	case Qt::Key_Enter:
	case Qt::Key_Return:
		if(selected)
		{
			selected->defaultAction();
			return;
		}
		break;
	case Qt::Key_Menu:
		if(selected)
			selected->showMenu(selected->mapToGlobal(selected->rect().bottomLeft()));
		else
			showMenu(mapToGlobal(QPoint()));
		break;
	}
	QScrollArea::keyPressEvent(event);
}

}
