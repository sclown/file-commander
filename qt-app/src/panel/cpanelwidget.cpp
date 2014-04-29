#include "cpanelwidget.h"
#include "filelistwidget/cfilelistview.h"
#include "filelistwidget/model/cfilelistmodel.h"
#include "ui_cpanelwidget.h"
#include "qflowlayout.h"
#include "shell/cshell.h"
#include "columns.h"
#include "filelistwidget/model/cfilelistsortfilterproxymodel.h"

#include <assert.h>
#include <time.h>
#include <set>

CPanelWidget::CPanelWidget(QWidget *parent /* = 0 */) :
	QWidget(parent),
	ui(new Ui::CPanelWidget),
	_controller (CController::get()),
	_selectionModel(0),
	_model(0),
	_sortModel(0),
	_panelPosition(UnknownPanel),
	_shiftPressed (false),
	_calcDirSizeShortcut(QKeySequence(Qt::Key_Space), this, SLOT(calcDirectorySize()), 0, Qt::WidgetWithChildrenShortcut),
	_selectCurrentItemShortcut(QKeySequence(Qt::Key_Insert), this, SLOT(invertCurrentItemSelection()), 0, Qt::WidgetWithChildrenShortcut)
{
	ui->setupUi(this);
#ifndef _WIN32
	connect(ui->_list, SIGNAL(doubleClicked(QModelIndex)), SLOT(itemActivatedSlot(QModelIndex)));
#else
	connect(ui->_list, SIGNAL(activated(QModelIndex)), SLOT(itemActivatedSlot(QModelIndex)));
#endif
	connect(ui->_list, SIGNAL(contextMenuRequested(QPoint)), SLOT(showContextMenuForItems(QPoint)));

	connect(ui->_pathNavigator, SIGNAL(returnPressed()), SLOT(onFolderPathSet()));

	_controller->setDisksChangedListener(this);
}

CPanelWidget::~CPanelWidget()
{
	delete ui;
}

void CPanelWidget::setFocusToFileList()
{
	ui->_list->setFocus();
}

QByteArray CPanelWidget::savePanelState() const
{
	return ui->_list->header()->saveState();
}

bool CPanelWidget::restorePanelState(QByteArray state)
{
	if (!state.isEmpty())
	{
		ui->_list->setHeaderAdjustmentRequired(false);
		return ui->_list->header()->restoreState(state);
	}
	else
	{
		ui->_list->setHeaderAdjustmentRequired(true);
		return false;
	}
}

QByteArray CPanelWidget::savePanelGeometry() const
{
	return ui->_list->header()->saveGeometry();
}

bool CPanelWidget::restorePanelGeometry(QByteArray state)
{
	return ui->_list->header()->restoreGeometry(state);
}

QString CPanelWidget::currentDir() const
{
	return ui->_pathNavigator->text();
}

Panel CPanelWidget::panelPosition() const
{
	return _panelPosition;
}

void CPanelWidget::setPanelPosition(Panel p)
{
	assert(_panelPosition == UnknownPanel);
	_panelPosition = p;

	ui->_list->installEventFilter(this);
	ui->_list->setPanelPosition(p);

	_model = new(std::nothrow) CFileListModel(ui->_list, this);
	_model->setPanelPosition(p);
	_sortModel = new(std::nothrow) CFileListSortFilterProxyModel(this);
	_sortModel->setPanelPosition(p);
	_sortModel->setSourceModel(_model);

	ui->_list->setModel(_sortModel);
	connect(_sortModel, SIGNAL(modelAboutToBeReset()), ui->_list, SLOT(modelAboutToBeReset()));
	_selectionModel = ui->_list->selectionModel(); // can only be called after setModel
	assert(_selectionModel);

	_controller->setPanelContentsChangedListener(this);
}

// Returns the list of items added to the view
void CPanelWidget::fillFromList(const std::vector<CFileSystemObject> &items)
{
	const time_t start = clock();

	// Remembering currently highlighted item to restore cursor afterwards
	const qulonglong currentItemHash = hashByItemIndex(_selectionModel->currentIndex());
	const int currentItemRow = std::max(_selectionModel->currentIndex().row(), 0);

	QList<QStandardItem*> itemsToAdd;
	ui->_list->saveHeaderState();
	_model->clear();
	_sortModel->setSourceModel(0);

	_model->setColumnCount(NumberOfColumns);
	_model->setHorizontalHeaderLabels(QStringList() << "Name" << "Ext" << "Size" << "Date");

	for (int i = 0; i < (int)items.size(); ++i)
	{
		auto props = items[i].properties();


		QStandardItem * fileNameItem = new QStandardItem();
		fileNameItem->setEditable(false);
		if (props.type == Directory)
			fileNameItem->setData(QString("[%1]").arg(props.name), Qt::DisplayRole);
		else if (props.name.isEmpty() && props.type == File) // File without a name, displaying extension in the name field and adding point to extension
			fileNameItem->setData(QString('.') + props.extension, Qt::DisplayRole);
		else
			fileNameItem->setData(props.name, Qt::DisplayRole);
		fileNameItem->setIcon(items[i].icon());
		fileNameItem->setData(props.hash, Qt::UserRole); // Unique identifier for this object;
		_model->setItem(i, NameColumn, fileNameItem);

		QStandardItem * fileExtItem = new QStandardItem();
		fileExtItem->setEditable(false);
		if (!props.name.isEmpty() && !props.extension.isEmpty())
			fileExtItem->setData(props.extension, Qt::DisplayRole);
		fileExtItem->setData(props.hash, Qt::UserRole); // Unique identifier for this object;
		_model->setItem(i, ExtColumn, fileExtItem);

		QStandardItem * sizeItem = new QStandardItem();
		sizeItem->setEditable(false);
		if (props.type != Directory)
			sizeItem->setData(fileSizeToString(props.size), Qt::DisplayRole);
		sizeItem->setData(props.hash, Qt::UserRole); // Unique identifier for this object;
		_model->setItem(i, SizeColumn, sizeItem);

		QStandardItem * dateItem = new QStandardItem();
		dateItem->setEditable(false);
		QDateTime modificationDate;
		modificationDate.setTime_t((uint)props.modificationDate);
		modificationDate = modificationDate.toLocalTime();
		dateItem->setData(modificationDate.toString("dd.MM.yyyy hh:mm"), Qt::DisplayRole);
		dateItem->setData(props.hash, Qt::UserRole); // Unique identifier for this object;
		_model->setItem(i, DateColumn, dateItem);
	}

	_sortModel->setSourceModel(_model);
	ui->_list->restoreHeaderState();

	bool cursorSet = false;
	if (currentItemHash != 0)
	{
		const QModelIndex currentIndex = indexByHash(currentItemHash);
		if (currentIndex.isValid())
		{
			ui->_list->moveCursorToItem(currentIndex);
			cursorSet = true;
		}
	}
	if (!cursorSet)
	{
		const qulonglong lastVisitedItemInDirectory = _panelPosition == UnknownPanel ? 0 : _controller->currentItemInFolder(_panelPosition, _controller->panel(_panelPosition).currentDirPath());
		if (lastVisitedItemInDirectory != 0)
		{
			const QModelIndex lastVisitedIndex = indexByHash(lastVisitedItemInDirectory);
			if (lastVisitedIndex.isValid())
				ui->_list->moveCursorToItem(lastVisitedIndex);
		}
		else
			ui->_list->moveCursorToItem(_sortModel->index(std::min(currentItemRow, _sortModel->rowCount()-1), 0));
	}

	qDebug () << __FUNCTION__ << "time = " << (clock() - start) * 1000 / CLOCKS_PER_SEC << " ms";
}

void CPanelWidget::fillFromPanel(const CPanel &panel)
{
	auto& itemList = panel.list();
	const auto previousSelection = selectedItemsHashes();
	std::set<qulonglong> selectedItemsHashes; // For fast search
	for (auto hash = previousSelection.begin(); hash != previousSelection.end(); ++hash)
		selectedItemsHashes.insert(*hash);

	fillFromList(itemList);

	// Restoring previous selection
	for (int row = 0; row < _sortModel->rowCount(); ++row)
	{
		const qulonglong hash = hashByItemRow(row);
		if (selectedItemsHashes.count(hash) != 0)
			_selectionModel->select(_sortModel->index(row, 0), QItemSelectionModel::Rows | QItemSelectionModel::Select);
	}

	_currentPath = panel.currentDirPath();
	const QString sep = toNativeSeparators("/");
	if (!_currentPath.endsWith(sep))
		ui->_pathNavigator->setText(_currentPath + sep);
	else
		ui->_pathNavigator->setText(_currentPath);
}

void CPanelWidget::itemActivatedSlot(QModelIndex item)
{
	assert(item.isValid());
	QModelIndex source = _sortModel->mapToSource(item);
	const qulonglong hash = _model->item(source.row(), source.column())->data(Qt::UserRole).toULongLong();
	emit itemActivated(hash, this);
}

void CPanelWidget::showContextMenuForItems(QPoint pos)
{
	const auto selection = selectedItemsHashes();
	std::vector<std::wstring> paths;
	if (selection.empty())
		paths.push_back(_controller->panel(_panelPosition).currentDirPath().toStdWString());
	else
	{
		for (size_t i = 0; i < selection.size(); ++i)
		{
			if (!_controller->itemByHash(_panelPosition, selection[i]).isCdUp() || selection.size() == 1)
			{
				QString selectedItemPath = _controller->itemPath(_panelPosition, selection[i]);
				paths.push_back(selectedItemPath.toStdWString());
			}
			else if (!selection.empty())
			{
				// This is a cdup element ([..]), and we should remove selection from it
				_selectionModel->select(indexByHash(selection[i]), QItemSelectionModel::Clear | QItemSelectionModel::Rows);
			}
		}
	}

	CShell::openShellContextMenuForObjects(paths, pos.x(), pos.y(), (void*)winId());
}

void CPanelWidget::showContextMenuForDisk(QPoint pos)
{
#ifdef _WIN32
	const QPushButton * button = dynamic_cast<const QPushButton*>(sender());
	if (!button)
		return;

	pos = button->mapToGlobal(pos);
	const size_t diskId = size_t(button->property("id").toUInt());
	std::vector<std::wstring> diskPath(1, _controller->diskPath(diskId).toStdWString());
	CShell::openShellContextMenuForObjects(diskPath, pos.x(), pos.y(), (HWND)winId());
#else
	Q_UNUSED(pos);
#endif
}

void CPanelWidget::onFolderPathSet()
{
	emit folderPathSet(toPosixSeparators(ui->_pathNavigator->text()), this);
}

void CPanelWidget::calcDirectorySize()
{
	QModelIndex itemIndex = _selectionModel->currentIndex();
	if (itemIndex.isValid())
	{
		_selectionModel->select(itemIndex, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
		_controller->calculateDirSize(_panelPosition, itemIndex.row());
	}
}

void CPanelWidget::invertCurrentItemSelection()
{
	const QAbstractItemModel * model = _selectionModel->model();
	QModelIndex item = _selectionModel->currentIndex();
	QModelIndex next = model->index(item.row() + 1, 0);
	if (item.isValid())
		_selectionModel->select(item, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
	if (next.isValid())
		ui->_list->moveCursorToItem(next);
}

void CPanelWidget::driveButtonClicked()
{
	if (!sender())
		return;

	const size_t id = size_t(sender()->property("id").toUInt());
	_controller->diskSelected(_panelPosition, id);
}

std::vector<qulonglong> CPanelWidget::selectedItemsHashes() const
{
	auto selection = _selectionModel->selectedRows();
	std::vector<qulonglong> result;

	if (!selection.empty())
	{
		for (auto it = selection.begin(); it != selection.end(); ++it)
		{
			const qulonglong hash = hashByItemIndex(*it);
			result.push_back(hash);
		}
	}
	else
	{
		auto currentIndex = _selectionModel->currentIndex();
		if (currentIndex.isValid())
			result.push_back(hashByItemIndex(currentIndex));
	}

	return result;
}

void CPanelWidget::disksChanged(std::vector<CDiskEnumerator::Drive> drives, Panel p, size_t currentDriveIndex)
{
	if (p != _panelPosition)
		return;

	if (!ui->_driveButtonsWidget->layout())
	{
		QFlowLayout * flowLayout = new QFlowLayout(ui->_driveButtonsWidget, 0, 0, 0);
		flowLayout->setSpacing(1);
		ui->_driveButtonsWidget->setLayout(flowLayout);
	}

	// Clearing and deleting the previous buttons
	QLayout * layout = ui->_driveButtonsWidget->layout();
	assert(layout);
	while (layout->count() > 0)
	{
		QWidget * w = layout->itemAt(0)->widget();
		layout->removeWidget(w);
		w->deleteLater();
	}

	// Creating and adding new buttons
	for (size_t i = 0; i < drives.size(); ++i)
	{
		const QString name = drives[i].displayName;

		assert(layout);
		QPushButton * diskButton = new QPushButton;
		diskButton->setCheckable(true);
		diskButton->setIcon(drives[i].fileSystemObject.icon());
		diskButton->setText(name);
		diskButton->setFixedWidth(QFontMetrics(diskButton->font()).width(diskButton->text()) + 5 + diskButton->iconSize().width() + 20);
		diskButton->setProperty("id", quint64(i));
		diskButton->setContextMenuPolicy(Qt::CustomContextMenu);
		diskButton->setToolTip(drives[i].detailedDescription);
		connect(diskButton, SIGNAL(clicked()), SLOT(driveButtonClicked()));
		connect(diskButton, SIGNAL(customContextMenuRequested(QPoint)), SLOT(showContextMenuForDisk(QPoint)));
		if (i == currentDriveIndex)
			diskButton->setChecked(true);
		layout->addWidget(diskButton);
	}
}

qulonglong CPanelWidget::hashByItemIndex(const QModelIndex &index) const
{
	if (!index.isValid())
		return 0;
	QStandardItem * item = _model->item(_sortModel->mapToSource(index).row(), 0);
	assert(item);
	bool ok = false;
	const qulonglong hash = item->data(Qt::UserRole).toULongLong(&ok);
	assert(ok);
	return hash;
}

qulonglong CPanelWidget::hashByItemRow(const int row) const
{
	return hashByItemIndex(_sortModel->index(row, 0));
}

QModelIndex CPanelWidget::indexByHash(const qulonglong hash) const
{
	for(int row = 0; row < _sortModel->rowCount(); ++row)
		if (hashByItemRow(row) == hash)
			return _sortModel->index(row, 0);
	return QModelIndex();
}

bool CPanelWidget::eventFilter(QObject * object, QEvent * e)
{
	if (object == ui->_list)
	{
		switch (e->type())
		{
		case QEvent::KeyPress:
		{
			QKeyEvent * keyEvent = dynamic_cast<QKeyEvent*>(e);
			if (keyEvent && keyEvent->key() == Qt::Key_Backspace)
			{
				// Navigating back
				emit backSpacePressed(this);
				return true;
			}
			else if (keyEvent && keyEvent->key() == Qt::Key_Shift)
			{
				_shiftPressed = true;
				return true;
			}
		}
			break;
		case QEvent::KeyRelease:
		{
			QKeyEvent * keyEvent = dynamic_cast<QKeyEvent*>(e);
			if (keyEvent && keyEvent->key() == Qt::Key_Shift)
			{
				_shiftPressed = false;
				return true;
			}
		}
			break;
		case QEvent::FocusOut:
			_shiftPressed = false;
			break;
		case QEvent::FocusIn:
			emit focusReceived(this);
			break;
		case QEvent::Wheel:
		{
			QWheelEvent * wEvent = dynamic_cast<QWheelEvent*>(e);
			if (wEvent && _shiftPressed)
			{
				if (wEvent->delta() > 0)
					emit stepBackRequested(this);
				else
					emit stepForwardRequested(this);
				return true;
			}
		}
			break;
		case QEvent::ContextMenu:
			showContextMenuForItems(QCursor::pos()); // QCursor::pos() returns global pos
			return true;
			break;
		default:
			break;
		}
	}
	return QWidget::eventFilter(object, e);
}

void CPanelWidget::panelContentsChanged( Panel p )
{
	if (p == _panelPosition)
		fillFromPanel(_controller->panel(_panelPosition));
}