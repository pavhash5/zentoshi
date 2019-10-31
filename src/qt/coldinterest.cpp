// Copyright (c) 2011-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/zentoshi-config.h>
#endif

#include <qt/coincontroldialog.h>
#include <qt/forms/ui_coincontroldialog.h>

#include <qt/addresstablemodel.h>
#include <base58.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <txmempool.h>
#include <qt/walletmodel.h>

#include <wallet/coincontrol.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <validation.h> // For mempool
#include <wallet/fees.h>
#include <wallet/wallet.h>

#include <instantx.h>
#include <privatesend/privatesend-client.h>

#include <QApplication>
#include <QCheckBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QFlags>
#include <QIcon>
#include <QSettings>
#include <QTreeWidget>

QList<CAmount> ColdInterestDialog::payAmounts;
bool ColdInterestDialog::fSubtractFeeFromAmount = false;

bool CColdInterestWidgetItem::operator<(const QTreeWidgetItem &other) const {
    int column = treeWidget()->sortColumn();
    if (column == ColdInterestDialog::COLUMN_AMOUNT || column == ColdInterestDialog::COLUMN_DATE || column == ColdInterestDialog::COLUMN_CONFIRMATIONS)
        return data(column, Qt::UserRole).toLongLong() < other.data(column, Qt::UserRole).toLongLong();
    return QTreeWidgetItem::operator<(other);
}

ColdInterestDialog::ColdInterestDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ColdInterestDialog),
    model(nullptr),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    // context menu actions
    QAction *copyAddressAction = new QAction(tr("Copy address"), this);
    QAction *copyLabelAction = new QAction(tr("Copy label"), this);
    QAction *copyAmountAction = new QAction(tr("Copy amount"), this);
             copyTransactionHashAction = new QAction(tr("Copy transaction ID"), this);  // we need to enable/disable this
             lockAction = new QAction(tr("Lock unspent"), this);                        // we need to enable/disable this
             unlockAction = new QAction(tr("Unlock unspent"), this);                    // we need to enable/disable this

    // context menu
    contextMenu = new QMenu(this);
    contextMenu->addAction(copyAddressAction);
    contextMenu->addAction(copyLabelAction);
    contextMenu->addAction(copyAmountAction);
    contextMenu->addAction(copyTransactionHashAction);
    contextMenu->addSeparator();
    contextMenu->addAction(lockAction);
    contextMenu->addAction(unlockAction);

    // context menu signals
    connect(ui->treeWidget, &QWidget::customContextMenuRequested, this, &ColdInterestDialog::showMenu);
    connect(copyAddressAction, &QAction::triggered, this, &ColdInterestDialog::copyAddress);
    connect(copyLabelAction, &QAction::triggered, this, &ColdInterestDialog::copyLabel);
    connect(copyAmountAction, &QAction::triggered, this, &ColdInterestDialog::copyAmount);
    connect(copyTransactionHashAction, &QAction::triggered, this, &ColdInterestDialog::copyTransactionHash);
    connect(lockAction, &QAction::triggered, this, &ColdInterestDialog::lockCoin);
    connect(unlockAction, &QAction::triggered, this, &ColdInterestDialog::unlockCoin);

    // clipboard actions
    QAction *clipboardQuantityAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardFeeAction = new QAction(tr("Copy fee"), this);
    QAction *clipboardAfterFeeAction = new QAction(tr("Copy after fee"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    QAction *clipboardLowOutputAction = new QAction(tr("Copy dust"), this);
    QAction *clipboardChangeAction = new QAction(tr("Copy change"), this);

    connect(clipboardQuantityAction, &QAction::triggered, this, &ColdInterestDialog::clipboardQuantity);
    connect(clipboardAmountAction, &QAction::triggered, this, &ColdInterestDialog::clipboardAmount);
    connect(clipboardFeeAction, &QAction::triggered, this, &ColdInterestDialog::clipboardFee);
    connect(clipboardAfterFeeAction, &QAction::triggered, this, &ColdInterestDialog::clipboardAfterFee);
    connect(clipboardBytesAction, &QAction::triggered, this, &ColdInterestDialog::clipboardBytes);
    connect(clipboardLowOutputAction, &QAction::triggered, this, &ColdInterestDialog::clipboardLowOutput);
    connect(clipboardChangeAction, &QAction::triggered, this, &ColdInterestDialog::clipboardChange);

    ui->labelColdInterestQuantity->addAction(clipboardQuantityAction);
    ui->labelColdInterestAmount->addAction(clipboardAmountAction);
    ui->labelColdInterestFee->addAction(clipboardFeeAction);
    ui->labelColdInterestAfterFee->addAction(clipboardAfterFeeAction);
    ui->labelColdInterestBytes->addAction(clipboardBytesAction);
    ui->labelColdInterestLowOutput->addAction(clipboardLowOutputAction);
    ui->labelColdInterestChange->addAction(clipboardChangeAction);

    // toggle tree/list mode
    connect(ui->radioTreeMode, &QRadioButton::toggled, this, &ColdInterestDialog::radioTreeMode);
    connect(ui->radioListMode, &QRadioButton::toggled, this, &ColdInterestDialog::radioListMode);

    // click on checkbox
    connect(ui->treeWidget, &QTreeWidget::itemChanged, this, &ColdInterestDialog::viewItemChanged);

    // click on header
    ui->treeWidget->header()->setSectionsClickable(true);
    connect(ui->treeWidget->header(), &QHeaderView::sectionClicked, this, &ColdInterestDialog::headerSectionClicked);

    // ok button
    connect(ui->buttonBox, &QDialogButtonBox::clicked, this, &ColdInterestDialog::buttonBoxClicked);

    // (un)select all
    connect(ui->pushButtonSelectAll, &QPushButton::clicked, this, &ColdInterestDialog::buttonSelectAllClicked);

    ui->treeWidget->setColumnWidth(COLUMN_CHECKBOX, 84);
    ui->treeWidget->setColumnWidth(COLUMN_AMOUNT, 110);
    ui->treeWidget->setColumnWidth(COLUMN_LABEL, 190);
    ui->treeWidget->setColumnWidth(COLUMN_ADDRESS, 320);
    ui->treeWidget->setColumnWidth(COLUMN_DATE, 130);
    ui->treeWidget->setColumnWidth(COLUMN_CONFIRMATIONS, 110);
    ui->treeWidget->setColumnHidden(COLUMN_TXHASH, true);         // store transaction hash in this column, but don't show it
    ui->treeWidget->setColumnHidden(COLUMN_VOUT_INDEX, true);     // store vout index in this column, but don't show it

    // default view is sorted by amount desc
    sortView(COLUMN_AMOUNT, Qt::DescendingOrder);

    // restore list mode and sortorder as a convenience feature
    QSettings settings;
    if (settings.contains("nColdInterestMode") && !settings.value("nColdInterestMode").toBool())
        ui->radioTreeMode->click();
    if (settings.contains("nColdInterestSortColumn") && settings.contains("nColdInterestSortOrder"))
        sortView(settings.value("nColdInterestSortColumn").toInt(), (static_cast<Qt::SortOrder>(settings.value("nColdInterestSortOrder").toInt())));
}

ColdInterestDialog::~ColdInterestDialog()
{
    QSettings settings;
    settings.setValue("nColdInterestMode", ui->radioListMode->isChecked());
    settings.setValue("nColdInterestSortColumn", sortColumn);
    settings.setValue("nColdInterestSortOrder", (int)sortOrder);

    delete ui;
}

void ColdInterestDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel() && _model->getAddressTableModel())
    {
        updateView();
        updateLabelLocked();
        ColdInterestDialog::updateLabels(_model, this);
    }
}

// ok button
void ColdInterestDialog::buttonBoxClicked(QAbstractButton* button)
{
    if (ui->buttonBox->buttonRole(button) == QDialogButtonBox::AcceptRole)
        done(QDialog::Accepted); // closes the dialog
}

// (un)select all
void ColdInterestDialog::buttonSelectAllClicked()
{
    Qt::CheckState state = Qt::Checked;
    for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++)
    {
        if (ui->treeWidget->topLevelItem(i)->checkState(COLUMN_CHECKBOX) != Qt::Unchecked)
        {
            state = Qt::Unchecked;
            break;
        }
    }
    ui->treeWidget->setEnabled(false);
    for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++)
            if (ui->treeWidget->topLevelItem(i)->checkState(COLUMN_CHECKBOX) != state)
                ui->treeWidget->topLevelItem(i)->setCheckState(COLUMN_CHECKBOX, state);
    ui->treeWidget->setEnabled(true);
    if (state == Qt::Unchecked)
        coinControl()->UnSelectAll(); // just to be sure
    ColdInterestDialog::updateLabels(model, this);
}

// context menu
void ColdInterestDialog::showMenu(const QPoint &point)
{
    QTreeWidgetItem *item = ui->treeWidget->itemAt(point);
    if(item)
    {
        contextMenuItem = item;

        // disable some items (like Copy Transaction ID, lock, unlock) for tree roots in context menu
        if (item->data(COLUMN_ADDRESS, TxHashRole).toString().length() == 64) // transaction hash is 64 characters (this means it is a child node, so it is not a parent node in tree mode)
        {
            copyTransactionHashAction->setEnabled(true);
            if (model->wallet().isLockedCoin(COutPoint(uint256S(item->data(COLUMN_ADDRESS, TxHashRole).toString().toStdString()), item->data(COLUMN_ADDRESS, VOutRole).toUInt())))
            {
                lockAction->setEnabled(false);
                unlockAction->setEnabled(true);
            }
            else
            {
                lockAction->setEnabled(true);
                unlockAction->setEnabled(false);
            }
        }
        else // this means click on parent node in tree mode -> disable all
        {
            copyTransactionHashAction->setEnabled(false);
            lockAction->setEnabled(false);
            unlockAction->setEnabled(false);
        }

        // show context menu
        contextMenu->exec(QCursor::pos());
    }
}

// context menu action: copy amount
void ColdInterestDialog::copyAmount()
{
    GUIUtil::setClipboard(BitcoinUnits::removeSpaces(contextMenuItem->text(COLUMN_AMOUNT)));
}

// context menu action: copy label
void ColdInterestDialog::copyLabel()
{
    if (ui->radioTreeMode->isChecked() && contextMenuItem->text(COLUMN_LABEL).length() == 0 && contextMenuItem->parent())
        GUIUtil::setClipboard(contextMenuItem->parent()->text(COLUMN_LABEL));
    else
        GUIUtil::setClipboard(contextMenuItem->text(COLUMN_LABEL));
}

// context menu action: copy address
void ColdInterestDialog::copyAddress()
{
    if (ui->radioTreeMode->isChecked() && contextMenuItem->text(COLUMN_ADDRESS).length() == 0 && contextMenuItem->parent())
        GUIUtil::setClipboard(contextMenuItem->parent()->text(COLUMN_ADDRESS));
    else
        GUIUtil::setClipboard(contextMenuItem->text(COLUMN_ADDRESS));
}

// context menu action: copy transaction id
void ColdInterestDialog::copyTransactionHash()
{
    GUIUtil::setClipboard(contextMenuItem->data(COLUMN_ADDRESS, TxHashRole).toString());
}

// context menu action: lock coin
void ColdInterestDialog::lockCoin()
{
    if (contextMenuItem->checkState(COLUMN_CHECKBOX) == Qt::Checked)
        contextMenuItem->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);

    COutPoint outpt(uint256S(contextMenuItem->data(COLUMN_ADDRESS, TxHashRole).toString().toStdString()), contextMenuItem->data(COLUMN_ADDRESS, VOutRole).toUInt());
    model->wallet().lockCoin(outpt);
    contextMenuItem->setDisabled(true);
    contextMenuItem->setIcon(COLUMN_CHECKBOX, platformStyle->SingleColorIcon(":/icons/lock_closed"));
    updateLabelLocked();
}

// context menu action: unlock coin
void ColdInterestDialog::unlockCoin()
{
    COutPoint outpt(uint256S(contextMenuItem->data(COLUMN_ADDRESS, TxHashRole).toString().toStdString()), contextMenuItem->data(COLUMN_ADDRESS, VOutRole).toUInt());
    model->wallet().unlockCoin(outpt);
    contextMenuItem->setDisabled(false);
    contextMenuItem->setIcon(COLUMN_CHECKBOX, QIcon());
    updateLabelLocked();
}

// copy label "Quantity" to clipboard
void ColdInterestDialog::clipboardQuantity()
{
    GUIUtil::setClipboard(ui->labelColdInterestQuantity->text());
}

// copy label "Amount" to clipboard
void ColdInterestDialog::clipboardAmount()
{
    GUIUtil::setClipboard(ui->labelColdInterestAmount->text().left(ui->labelColdInterestAmount->text().indexOf(" ")));
}

// copy label "Fee" to clipboard
void ColdInterestDialog::clipboardFee()
{
    GUIUtil::setClipboard(ui->labelColdInterestFee->text().left(ui->labelColdInterestFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// copy label "After fee" to clipboard
void ColdInterestDialog::clipboardAfterFee()
{
    GUIUtil::setClipboard(ui->labelColdInterestAfterFee->text().left(ui->labelColdInterestAfterFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// copy label "Bytes" to clipboard
void ColdInterestDialog::clipboardBytes()
{
    GUIUtil::setClipboard(ui->labelColdInterestBytes->text().replace(ASYMP_UTF8, ""));
}

// copy label "Dust" to clipboard
void ColdInterestDialog::clipboardLowOutput()
{
    GUIUtil::setClipboard(ui->labelColdInterestLowOutput->text());
}

// copy label "Change" to clipboard
void ColdInterestDialog::clipboardChange()
{
    GUIUtil::setClipboard(ui->labelColdInterestChange->text().left(ui->labelColdInterestChange->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// treeview: sort
void ColdInterestDialog::sortView(int column, Qt::SortOrder order)
{
    sortColumn = column;
    sortOrder = order;
    ui->treeWidget->sortItems(column, order);
    ui->treeWidget->header()->setSortIndicator(sortColumn, sortOrder);
}

// treeview: clicked on header
void ColdInterestDialog::headerSectionClicked(int logicalIndex)
{
    if (logicalIndex == COLUMN_CHECKBOX) // click on most left column -> do nothing
    {
        ui->treeWidget->header()->setSortIndicator(sortColumn, sortOrder);
    }
    else
    {
        if (sortColumn == logicalIndex)
            sortOrder = ((sortOrder == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder);
        else
        {
            sortColumn = logicalIndex;
            sortOrder = ((sortColumn == COLUMN_LABEL || sortColumn == COLUMN_ADDRESS) ? Qt::AscendingOrder : Qt::DescendingOrder); // if label or address then default => asc, else default => desc
        }

        sortView(sortColumn, sortOrder);
    }
}

// toggle tree mode
void ColdInterestDialog::radioTreeMode(bool checked)
{
    if (checked && model)
        updateView();
}

// toggle list mode
void ColdInterestDialog::radioListMode(bool checked)
{
    if (checked && model)
        updateView();
}

// checkbox clicked by user
void ColdInterestDialog::viewItemChanged(QTreeWidgetItem* item, int column)
{
    if (column == COLUMN_CHECKBOX && item->data(COLUMN_ADDRESS, TxHashRole).toString().length() == 64) // transaction hash is 64 characters (this means it is a child node, so it is not a parent node in tree mode)
    {
        COutPoint outpt(uint256S(item->data(COLUMN_ADDRESS, TxHashRole).toString().toStdString()), item->data(COLUMN_ADDRESS, VOutRole).toUInt());

        if (item->checkState(COLUMN_CHECKBOX) == Qt::Unchecked)
            coinControl()->UnSelect(outpt);
        else if (item->isDisabled()) // locked (this happens if "check all" through parent node)
            item->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);
        else {
            coinControl()->Select(outpt);
            int nRounds = model->wallet().GetOutpointPrivateSendRounds(outpt);
            if (coinControl()->fUsePrivateSend && nRounds < privateSendClient.nPrivateSendRounds) {
                QMessageBox::warning(this, windowTitle(),
                    tr("Non-anonymized input selected. <b>PrivateSend will be disabled.</b><br><br>If you still want to use PrivateSend, please deselect all non-anonymized inputs first and then check the PrivateSend checkbox again."),
                    QMessageBox::Ok, QMessageBox::Ok);
                coinControl()->fUsePrivateSend = false;
            }
        }

        // selection changed -> update labels
        if (ui->treeWidget->isEnabled()) // do not update on every click for (un)select all
            ColdInterestDialog::updateLabels(model, this);
    }

    // TODO: Remove this temporary qt5 fix after Qt5.3 and Qt5.4 are no longer used.
    //       Fixed in Qt5.5 and above: https://bugreports.qt.io/browse/QTBUG-43473
    else if (column == COLUMN_CHECKBOX && item->childCount() > 0)
    {
        if (item->checkState(COLUMN_CHECKBOX) == Qt::PartiallyChecked && item->child(0)->checkState(COLUMN_CHECKBOX) == Qt::PartiallyChecked)
            item->setCheckState(COLUMN_CHECKBOX, Qt::Checked);
    }
}

// shows count of locked unspent outputs
void ColdInterestDialog::updateLabelLocked()
{
    std::vector<COutPoint> vOutpts;
    model->wallet().listLockedCoins(vOutpts);
    if (vOutpts.size() > 0)
    {
       ui->labelLocked->setText(tr("(%1 locked)").arg(vOutpts.size()));
       ui->labelLocked->setVisible(true);
    }
    else ui->labelLocked->setVisible(false);
}

void ColdInterestDialog::updateLabels(WalletModel *model, QDialog* dialog)
{
    if (!model)
        return;

    // nPayAmount
    CAmount nPayAmount = 0;
    bool fDust = false;
    CMutableTransaction txDummy;
    for (const CAmount &amount : ColdInterestDialog::payAmounts)
    {
        nPayAmount += amount;

        if (amount > 0)
        {
            CTxOut txout(amount, static_cast<CScript>(std::vector<unsigned char>(24, 0)));
            txDummy.vout.push_back(txout);
            fDust |= IsDust(txout, model->node().getDustRelayFee());
        }
    }

    CAmount nAmount             = 0;
    CAmount nPayFee             = 0;
    CAmount nAfterFee           = 0;
    CAmount nChange             = 0;
    unsigned int nBytes         = 0;
    unsigned int nBytesInputs   = 0;
    unsigned int nQuantity      = 0;
    bool fWitness               = false;

    std::vector<COutPoint> vColdInterest;
    coinControl()->ListSelected(vColdInterest);

    size_t i = 0;
    for (const auto& out : model->wallet().getCoins(vColdInterest)) {
        if (out.depth_in_main_chain < 0) continue;

        // unselect already spent, very unlikely scenario, this could happen
        // when selected are spent elsewhere, like rpc or another computer
        const COutPoint& outpt = vColdInterest[i++];
        if (out.is_spent)
        {
            coinControl()->UnSelect(outpt);
            continue;
        }

        // Quantity
        nQuantity++;

        // Amount
        nAmount += out.txout.nValue;

        // Bytes
        CTxDestination address;
        int witnessversion = 0;
        std::vector<unsigned char> witnessprogram;
        if (out.txout.scriptPubKey.IsWitnessProgram(witnessversion, witnessprogram))
        {
            nBytesInputs += (32 + 4 + 1 + (107 / WITNESS_SCALE_FACTOR) + 4);
            fWitness = true;
        }
        else if(ExtractDestination(out.txout.scriptPubKey, address))
        {
            CPubKey pubkey;
            CKeyID *keyid = boost::get<CKeyID>(&address);
            if (keyid && model->wallet().getPubKey(*keyid, pubkey))
            {
                nBytesInputs += (pubkey.IsCompressed() ? 148 : 180);
            }
            else
                nBytesInputs += 148; // in all error cases, simply assume 148 here
        }
        else nBytesInputs += 148;

        if(coinControl()->fUseInstantSend)
            txDummy.vin.push_back(CTxIn());
    }

    // calculation
    if (nQuantity > 0)
    {
        // Bytes
        nBytes = nBytesInputs + ((ColdInterestDialog::payAmounts.size() > 0 ? ColdInterestDialog::payAmounts.size() + 1 : 2) * 34) + 10; // always assume +1 output for change here
        if (fWitness)
        {
            // there is some fudging in these numbers related to the actual virtual transaction size calculation that will keep this estimate from being exact.
            // usually, the result will be an overestimate within a couple of satoshis so that the confirmation dialog ends up displaying a slightly smaller fee.
            // also, the witness stack size value is a variable sized integer. usually, the number of stack items will be well under the single byte var int limit.
            nBytes += 2; // account for the serialized marker and flag bytes
            nBytes += nQuantity; // account for the witness byte that holds the number of stack items for each input.
        }

        // in the subtract fee from amount case, we can tell if zero change already and subtract the bytes, so that fee calculation afterwards is accurate
        if (ColdInterestDialog::fSubtractFeeFromAmount)
            if (nAmount - nPayAmount == 0)
                nBytes -= 34;

        // Fee
        nPayFee = coinControl()->nMinimumTotalFee;

        // InstantSend Fee
        if (coinControl()->fUseInstantSend) nPayFee = std::max(nPayFee, CTxLockRequest(txDummy).GetMinFee(true));

        if (nPayAmount > 0)
        {
            nChange = nAmount - nPayAmount;
            if (!ColdInterestDialog::fSubtractFeeFromAmount)
                nChange -= nPayFee;

            // Never create dust outputs; if we would, just add the dust to the fee.
            if(coinControl()->fUsePrivateSend && nChange > 0)
            {
                nPayFee += nChange;
                nChange = 0;
            }
            if (nChange > 0 && nChange < MIN_CHANGE)
            {
                CTxOut txout(nChange, static_cast<CScript>(std::vector<unsigned char>(24, 0)));
                if (IsDust(txout, model->node().getDustRelayFee()))
                {
                    nPayFee += nChange;
                    nChange = 0;
                    if (ColdInterestDialog::fSubtractFeeFromAmount)
                        nBytes -= 34; // we didn't detect lack of change above
                }
            }

            if (nChange == 0 && !ColdInterestDialog::fSubtractFeeFromAmount)
                nBytes -= 34;
        }

        // after fee
        nAfterFee = std::max<CAmount>(nAmount - nPayFee, 0);
    }

    // actually update labels
    int nDisplayUnit = BitcoinUnits::ZENX;
    if (model && model->getOptionsModel())
        nDisplayUnit = model->getOptionsModel()->getDisplayUnit();

    QLabel *l1 = dialog->findChild<QLabel *>("labelColdInterestQuantity");
    QLabel *l2 = dialog->findChild<QLabel *>("labelColdInterestAmount");
    QLabel *l3 = dialog->findChild<QLabel *>("labelColdInterestFee");
    QLabel *l4 = dialog->findChild<QLabel *>("labelColdInterestAfterFee");
    QLabel *l5 = dialog->findChild<QLabel *>("labelColdInterestBytes");
    QLabel *l7 = dialog->findChild<QLabel *>("labelColdInterestLowOutput");
    QLabel *l8 = dialog->findChild<QLabel *>("labelColdInterestChange");

    // enable/disable "dust" and "change"
    dialog->findChild<QLabel *>("labelColdInterestLowOutputText")->setEnabled(nPayAmount > 0);
    dialog->findChild<QLabel *>("labelColdInterestLowOutput")    ->setEnabled(nPayAmount > 0);
    dialog->findChild<QLabel *>("labelColdInterestChangeText")   ->setEnabled(nPayAmount > 0);
    dialog->findChild<QLabel *>("labelColdInterestChange")       ->setEnabled(nPayAmount > 0);

    // stats
    l1->setText(QString::number(nQuantity));                                 // Quantity
    l2->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, nAmount));        // Amount
    l3->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, nPayFee));        // Fee
    l4->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, nAfterFee));      // After Fee
    l5->setText(((nBytes > 0) ? ASYMP_UTF8 : "") + QString::number(nBytes)); // Bytes
    l7->setText(fDust ? tr("yes") : tr("no"));                               // Dust
    l8->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, nChange));        // Change
    if (nPayFee > 0)
    {
        l3->setText(ASYMP_UTF8 + l3->text());
        l4->setText(ASYMP_UTF8 + l4->text());
        if (nChange > 0 && !ColdInterestDialog::fSubtractFeeFromAmount)
            l8->setText(ASYMP_UTF8 + l8->text());
    }

    // turn label red when dust
    l7->setStyleSheet((fDust) ? "color:red;" : "");

    // tool tips
    QString toolTipDust = tr("This label turns red if any recipient receives an amount smaller than the current dust threshold.");

    // how many satoshis the estimated fee can vary per byte we guess wrong
    double dFeeVary = (nBytes != 0) ? (double)nPayFee / nBytes : 0;

    QString toolTip4 = tr("Can vary +/- %1 satoshi(s) per input.").arg(dFeeVary);

    l3->setToolTip(toolTip4);
    l4->setToolTip(toolTip4);
    l7->setToolTip(toolTipDust);
    l8->setToolTip(toolTip4);
    dialog->findChild<QLabel *>("labelColdInterestFeeText")      ->setToolTip(l3->toolTip());
    dialog->findChild<QLabel *>("labelColdInterestAfterFeeText") ->setToolTip(l4->toolTip());
    dialog->findChild<QLabel *>("labelColdInterestBytesText")    ->setToolTip(l5->toolTip());
    dialog->findChild<QLabel *>("labelColdInterestLowOutputText")->setToolTip(l7->toolTip());
    dialog->findChild<QLabel *>("labelColdInterestChangeText")   ->setToolTip(l8->toolTip());

    // Insufficient funds
    QLabel *label = dialog->findChild<QLabel *>("labelColdInterestInsuffFunds");
    if (label)
        label->setVisible(nChange < 0);
}

CColdInterest* ColdInterestDialog::coinControl()
{
    static CColdInterest coin_control;
    return &coin_control;
}

void ColdInterestDialog::updateView()
{
    if (!model || !model->getOptionsModel() || !model->getAddressTableModel())
        return;

    bool treeMode = ui->radioTreeMode->isChecked();

    ui->treeWidget->clear();
    ui->treeWidget->setEnabled(false); // performance, otherwise updateLabels would be called for every checked checkbox
    ui->treeWidget->setAlternatingRowColors(!treeMode);
    QFlags<Qt::ItemFlag> flgCheckbox = Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
    QFlags<Qt::ItemFlag> flgTristate = Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsTristate;

    int nDisplayUnit = model->getOptionsModel()->getDisplayUnit();

    for (const auto& coins : model->wallet().listCoins()) {
        CColdInterestWidgetItem *itemWalletAddress = new CColdInterestWidgetItem();
        itemWalletAddress->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);
        QString sWalletAddress = QString::fromStdString(EncodeDestination(coins.first));
        QString sWalletLabel = model->getAddressTableModel()->labelForAddress(sWalletAddress);
        if (sWalletLabel.isEmpty())
            sWalletLabel = tr("(no label)");

        if (treeMode)
        {
            // wallet address
            ui->treeWidget->addTopLevelItem(itemWalletAddress);

            itemWalletAddress->setFlags(flgTristate);
            itemWalletAddress->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);

            // label
            itemWalletAddress->setText(COLUMN_LABEL, sWalletLabel);

            // address
            itemWalletAddress->setText(COLUMN_ADDRESS, sWalletAddress);
        }

        CAmount nSum = 0;
        int nChildren = 0;
        for (const auto& outpair : coins.second) {
            const COutPoint& output = std::get<0>(outpair);
            const interfaces::WalletTxOut& out = std::get<1>(outpair);
            nSum += out.txout.nValue;
            nChildren++;

            CColdInterestWidgetItem *itemOutput;
            if (treeMode)    itemOutput = new CColdInterestWidgetItem(itemWalletAddress);
            else             itemOutput = new CColdInterestWidgetItem(ui->treeWidget);
            itemOutput->setFlags(flgCheckbox);
            itemOutput->setCheckState(COLUMN_CHECKBOX,Qt::Unchecked);

            // address
            CTxDestination outputAddress;
            QString sAddress = "";
            if(ExtractDestination(out.txout.scriptPubKey, outputAddress))
            {
                sAddress = QString::fromStdString(EncodeDestination(outputAddress));

                // if listMode or change => show bitcoin address. In tree mode, address is not shown again for direct wallet address outputs
                if (!treeMode || (!(sAddress == sWalletAddress)))
                    itemOutput->setText(COLUMN_ADDRESS, sAddress);
            }

            // label
            if (!(sAddress == sWalletAddress)) // change
            {
                // tooltip from where the change comes from
                itemOutput->setToolTip(COLUMN_LABEL, tr("change from %1 (%2)").arg(sWalletLabel).arg(sWalletAddress));
                itemOutput->setText(COLUMN_LABEL, tr("(change)"));
            }
            else if (!treeMode)
            {
                QString sLabel = model->getAddressTableModel()->labelForAddress(sAddress);
                if (sLabel.isEmpty())
                    sLabel = tr("(no label)");
                itemOutput->setText(COLUMN_LABEL, sLabel);
            }

            // amount
            itemOutput->setText(COLUMN_AMOUNT, BitcoinUnits::format(nDisplayUnit, out.txout.nValue));
            itemOutput->setData(COLUMN_AMOUNT, Qt::UserRole, QVariant((qlonglong)out.txout.nValue)); // padding so that sorting works correctly

            // date
            itemOutput->setText(COLUMN_DATE, GUIUtil::dateTimeStr(out.time));
            itemOutput->setData(COLUMN_DATE, Qt::UserRole, QVariant((qlonglong)out.time));

            // confirmations
            itemOutput->setText(COLUMN_CONFIRMATIONS, QString::number(out.depth_in_main_chain));
            itemOutput->setData(COLUMN_CONFIRMATIONS, Qt::UserRole, QVariant((qlonglong)out.depth_in_main_chain));

            // transaction hash
            itemOutput->setData(COLUMN_ADDRESS, TxHashRole, QString::fromStdString(output.hash.GetHex()));

            // vout index
            itemOutput->setData(COLUMN_ADDRESS, VOutRole, output.n);

            // disable locked coins
            if (model->wallet().isLockedCoin(output))
            {
                coinControl()->UnSelect(output); // just to be sure
                itemOutput->setDisabled(true);
                itemOutput->setIcon(COLUMN_CHECKBOX, platformStyle->SingleColorIcon(":/icons/lock_closed"));
            }

            // set checkbox
            if (coinControl()->IsSelected(output))
                itemOutput->setCheckState(COLUMN_CHECKBOX, Qt::Checked);
        }

        // amount
        if (treeMode)
        {
            itemWalletAddress->setText(COLUMN_CHECKBOX, "(" + QString::number(nChildren) + ")");
            itemWalletAddress->setText(COLUMN_AMOUNT, BitcoinUnits::format(nDisplayUnit, nSum));
            itemWalletAddress->setData(COLUMN_AMOUNT, Qt::UserRole, QVariant((qlonglong)nSum));
        }
    }

    // expand all partially selected
    if (treeMode)
    {
        for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++)
            if (ui->treeWidget->topLevelItem(i)->checkState(COLUMN_CHECKBOX) == Qt::PartiallyChecked)
                ui->treeWidget->topLevelItem(i)->setExpanded(true);
    }

    // sort view
    sortView(sortColumn, sortOrder);
    ui->treeWidget->setEnabled(true);
}
