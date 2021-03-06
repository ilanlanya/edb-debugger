/*
Copyright (C) 2006 - 2015 Evan Teran
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

#ifndef DIALOG_HEADER_20111128_H_
#define DIALOG_HEADER_20111128_H_

#include "Types.h"
#include <QDialog>

class QStringListModel;
class QSortFilterProxyModel;
class QModelIndex;

namespace BinaryInfoPlugin {

namespace Ui { class DialogHeader; }

class DialogHeader : public QDialog {
	Q_OBJECT

public:
    explicit DialogHeader(QWidget *parent = nullptr);
	virtual ~DialogHeader();

public Q_SLOTS:
	void on_btnExplore_clicked();

private:
	virtual void showEvent(QShowEvent *event);

private:
	 Ui::DialogHeader *const ui;
	 QSortFilterProxyModel *filter_model_;
};

}

#endif
