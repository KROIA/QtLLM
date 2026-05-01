#include "ModelManagerDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QGroupBox>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QHeaderView>

ModelManagerDialog::ModelManagerDialog(QtLLM::OllamaManager* manager,
                                       const QString& currentModel,
                                       QWidget* parent)
    : QDialog(parent)
    , m_manager(manager)
    , m_currentModel(currentModel)
{
    setWindowTitle("Manage Ollama Models");
    setMinimumSize(520, 440);
    buildUi();

    connect(m_manager, &QtLLM::OllamaManager::localModelsReady,
            this, &ModelManagerDialog::onLocalModelsReady);
    connect(m_manager, &QtLLM::OllamaManager::pullProgress,
            this, &ModelManagerDialog::onPullProgress);
    connect(m_manager, &QtLLM::OllamaManager::pullFinished,
            this, &ModelManagerDialog::onPullFinished);

    m_manager->fetchLocalModels();
}

void ModelManagerDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    auto* tabs = new QTabWidget(this);

    // ── Tab 1: Installed models ───────────────────────────────────────────────
    auto* installedPage = new QWidget();
    auto* installedLayout = new QVBoxLayout(installedPage);

    m_installedList = new QListWidget();
    m_installedList->setAlternatingRowColors(true);
    installedLayout->addWidget(m_installedList);

    auto* installedBtnRow = new QHBoxLayout();
    m_refreshBtn = new QPushButton("Refresh");
    m_selectBtn  = new QPushButton("Use Selected Model");
    m_selectBtn->setEnabled(false);
    m_selectBtn->setDefault(true);
    installedBtnRow->addWidget(m_refreshBtn);
    installedBtnRow->addStretch();
    installedBtnRow->addWidget(m_selectBtn);
    installedLayout->addLayout(installedBtnRow);

    tabs->addTab(installedPage, "Installed");

    // ── Tab 2: Available models (download) ────────────────────────────────────
    auto* availablePage = new QWidget();
    auto* availableLayout = new QVBoxLayout(availablePage);

    m_availableList = new QListWidget();
    m_availableList->setAlternatingRowColors(true);
    availableLayout->addWidget(m_availableList);

    m_downloadBtn = new QPushButton("Download Selected");
    m_downloadBtn->setEnabled(false);
    availableLayout->addWidget(m_downloadBtn, 0, Qt::AlignRight);

    auto* customGroup = new QGroupBox("Pull any model by name");
    auto* customRow   = new QHBoxLayout(customGroup);
    m_customNameEdit  = new QLineEdit();
    m_customNameEdit->setPlaceholderText("e.g. llama3.2:latest");
    m_customPullBtn   = new QPushButton("Pull");
    customRow->addWidget(m_customNameEdit, 1);
    customRow->addWidget(m_customPullBtn);
    availableLayout->addWidget(customGroup);

    tabs->addTab(availablePage, "Download");

    root->addWidget(tabs);

    // ── Progress area (always visible at bottom) ──────────────────────────────
    auto* progressGroup = new QGroupBox("Download progress");
    auto* progressLayout = new QVBoxLayout(progressGroup);
    m_statusLabel  = new QLabel("Idle");
    m_progressBar  = new QProgressBar();
    m_progressBar->setRange(0, 100);
    m_progressBar->setTextVisible(true);
    progressLayout->addWidget(m_statusLabel);
    progressLayout->addWidget(m_progressBar);
    progressGroup->setVisible(false);
    root->addWidget(progressGroup);

    // Store reference so we can show/hide it
    m_progressBar->setProperty("group", QVariant::fromValue(static_cast<QObject*>(progressGroup)));

    // ── Close button ──────────────────────────────────────────────────────────
    auto* closeBtn = new QPushButton("Close");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    auto* closeRow = new QHBoxLayout();
    closeRow->addStretch();
    closeRow->addWidget(closeBtn);
    root->addLayout(closeRow);

    // Connections
    connect(m_installedList, &QListWidget::itemDoubleClicked,
            this, &ModelManagerDialog::onInstalledItemDoubleClicked);
    connect(m_installedList, &QListWidget::itemSelectionChanged,
            this, &ModelManagerDialog::onInstalledSelectionChanged);
    connect(m_availableList, &QListWidget::itemSelectionChanged,
            this, &ModelManagerDialog::onAvailableSelectionChanged);
    connect(m_selectBtn,     &QPushButton::clicked, this, &ModelManagerDialog::onSelectClicked);
    connect(m_downloadBtn,   &QPushButton::clicked, this, &ModelManagerDialog::onDownloadClicked);
    connect(m_refreshBtn,    &QPushButton::clicked, this, &ModelManagerDialog::onRefreshClicked);
    connect(m_customPullBtn, &QPushButton::clicked, this, &ModelManagerDialog::onCustomPullClicked);
}

void ModelManagerDialog::onLocalModelsReady(const QList<QtLLM::OllamaManager::ModelInfo>& models)
{
    m_localModels = models;

    m_installedList->clear();
    for (const auto& info : models) {
        QString sizeStr = QtLLM::OllamaManager::formatSize(info.sizeBytes);
        QString label   = info.name;
        if (!sizeStr.isEmpty()) label += QString("  (%1)").arg(sizeStr);
        auto* item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, info.name);
        if (info.name == m_currentModel)
            item->setForeground(Qt::darkGreen);
        m_installedList->addItem(item);
    }

    populateAvailableList(models);
}

void ModelManagerDialog::populateAvailableList(const QList<QtLLM::OllamaManager::ModelInfo>& installed)
{
    QSet<QString> installedNames;
    for (const auto& info : installed)
        installedNames.insert(info.name);

    m_availableList->clear();
    for (auto info : QtLLM::OllamaManager::popularModels()) {
        info.installed = installedNames.contains(info.name);
        QString sizeStr = QtLLM::OllamaManager::formatSize(info.sizeBytes);
        QString label   = info.displayName;
        if (!sizeStr.isEmpty()) label += QString("  (%1)").arg(sizeStr);

        auto* item = new QListWidgetItem();
        item->setData(Qt::UserRole, info.name);

        if (info.installed) {
            item->setText("✓  " + label);
            item->setForeground(Qt::darkGray);
            QFont f = item->font();
            f.setItalic(true);
            item->setFont(f);
            item->setToolTip("Already installed");
        } else {
            item->setText("↓  " + label);
            item->setToolTip(QString("Click Download to install '%1'").arg(info.name));
        }
        m_availableList->addItem(item);
    }
}

void ModelManagerDialog::onPullProgress(const QString& modelName, const QString& status, int percent)
{
    Q_UNUSED(modelName)
    m_statusLabel->setText(status);
    if (percent >= 0) {
        m_progressBar->setValue(percent);
    } else {
        // Indeterminate — just cycle the bar
        if (m_progressBar->maximum() != 0) {
            m_progressBar->setRange(0, 0);
        }
    }
}

void ModelManagerDialog::onPullFinished(const QString& modelName, bool success, const QString& errorMessage)
{
    setDownloading(false);

    if (success) {
        m_statusLabel->setText(QString("'%1' downloaded successfully.").arg(modelName));
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(100);
        // Refresh installed list
        m_manager->fetchLocalModels();
    } else {
        QMessageBox::warning(this, "Download failed",
                             QString("Failed to download '%1':\n%2").arg(modelName, errorMessage));
        m_statusLabel->setText("Download failed.");
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
    }
}

void ModelManagerDialog::onInstalledItemDoubleClicked(QListWidgetItem* item)
{
    if (!item) return;
    QString name = item->data(Qt::UserRole).toString();
    emit modelSelected(name);
    accept();
}

void ModelManagerDialog::onSelectClicked()
{
    auto selected = m_installedList->selectedItems();
    if (selected.isEmpty()) return;
    QString name = selected.first()->data(Qt::UserRole).toString();
    emit modelSelected(name);
    accept();
}

void ModelManagerDialog::onDownloadClicked()
{
    auto selected = m_availableList->selectedItems();
    if (selected.isEmpty()) return;
    QString name = selected.first()->data(Qt::UserRole).toString();

    setDownloading(true, name);
    m_manager->pullModel(name);
}

void ModelManagerDialog::onCustomPullClicked()
{
    QString name = m_customNameEdit->text().trimmed();
    if (name.isEmpty()) return;
    setDownloading(true, name);
    m_manager->pullModel(name);
}

void ModelManagerDialog::onRefreshClicked()
{
    m_installedList->clear();
    m_manager->fetchLocalModels();
}

void ModelManagerDialog::onInstalledSelectionChanged()
{
    m_selectBtn->setEnabled(!m_installedList->selectedItems().isEmpty());
}

void ModelManagerDialog::onAvailableSelectionChanged()
{
    auto sel = m_availableList->selectedItems();
    bool hasSelection = !sel.isEmpty();
    bool isInstalled  = hasSelection &&
        sel.first()->data(Qt::UserRole).toString().length() > 0 &&
        sel.first()->text().startsWith(QChar(0x2713)); // ✓
    m_downloadBtn->setEnabled(hasSelection && !isInstalled);
}

void ModelManagerDialog::setDownloading(bool active, const QString& modelName)
{
    // Show/hide the progress group
    auto* group = qobject_cast<QGroupBox*>(
        m_progressBar->property("group").value<QObject*>());
    if (group) group->setVisible(active || !modelName.isEmpty());

    m_downloadBtn->setEnabled(!active);
    m_customPullBtn->setEnabled(!active);
    m_refreshBtn->setEnabled(!active);

    if (active) {
        m_progressBar->setRange(0, 0);
        m_statusLabel->setText(QString("Starting download of '%1'…").arg(modelName));
    }
}
