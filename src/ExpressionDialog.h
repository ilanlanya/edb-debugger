/*
Copyright (C) 2014 - 2015 Evan Teran
                          evan.teran@gmail.com

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

#include "Types.h"

#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QPalette>

class QString;

class ExpressionDialog : public QDialog {
	Q_OBJECT

public:
    explicit ExpressionDialog(const QString &title, const QString &prompt);

public:
	edb::address_t getAddress();
	
private Q_SLOTS:
	void on_text_changed(const QString& text);
	
private:
	QVBoxLayout      layout_;
	QLabel           label_text_;
	QLabel           label_error_;
	QLineEdit        expression_;
	QDialogButtonBox button_box_;
	QPalette         palette_error_;
	edb::address_t   last_address_;
};
