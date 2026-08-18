#include "home/HomeWindow.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "project/ProjectFile.h"

namespace solidar::home {
namespace {

QFrame* makeActionCard(const QString& icon, const QString& title,
                       const QString& description, const QString& buttonText,
                       bool primary, QWidget* parent, QPushButton** button) {
  auto* card = new QFrame(parent);
  card->setObjectName("actionCard");
  card->setMinimumHeight(270);
  auto* layout = new QVBoxLayout(card);
  layout->setContentsMargins(32, 30, 32, 30);
  layout->setSpacing(14);

  auto* iconLabel = new QLabel(icon, card);
  iconLabel->setObjectName("cardIcon");
  iconLabel->setFixedSize(58, 58);
  iconLabel->setAlignment(Qt::AlignCenter);
  auto* titleLabel = new QLabel(title, card);
  titleLabel->setObjectName("cardTitle");
  auto* descriptionLabel = new QLabel(description, card);
  descriptionLabel->setObjectName("cardDescription");
  descriptionLabel->setWordWrap(true);
  auto* action = new QPushButton(buttonText, card);
  action->setObjectName(primary ? "primaryButton" : "secondaryButton");
  action->setMinimumHeight(48);
  action->setCursor(Qt::PointingHandCursor);

  layout->addWidget(iconLabel, 0, Qt::AlignLeft);
  layout->addWidget(titleLabel);
  layout->addWidget(descriptionLabel);
  layout->addStretch();
  layout->addWidget(action);
  *button = action;
  return card;
}

}  // namespace

HomeWindow::HomeWindow(QWidget* parent) : QMainWindow(parent) {
  buildUi();
  resize(1280, 760);
  setMinimumSize(960, 620);
  setWindowTitle(QString::fromUtf8("Солидарность 3D"));
}

void HomeWindow::buildUi() {
  auto* root = new QWidget(this);
  root->setObjectName("root");
  auto* rootLayout = new QHBoxLayout(root);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(0);

  auto* sidebar = new QFrame(root);
  sidebar->setObjectName("sidebar");
  sidebar->setFixedWidth(270);
  auto* sidebarLayout = new QVBoxLayout(sidebar);
  sidebarLayout->setContentsMargins(26, 30, 26, 28);
  sidebarLayout->setSpacing(18);

  auto* brand = new QWidget(sidebar);
  auto* brandLayout = new QVBoxLayout(brand);
  brandLayout->setContentsMargins(0, 0, 0, 20);
  auto* logo = new QLabel(brand);
  logo->setObjectName("brandLogo");
  logo->setFixedHeight(82);
  logo->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  const QPixmap logoPixmap(QStringLiteral(":/icons/solidarity-3d-logo.png"));
  logo->setPixmap(logoPixmap.scaled(218, 82, Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation));
  brandLayout->addWidget(logo);
  sidebarLayout->addWidget(brand);

  auto* overview = new QPushButton(
      QIcon(QStringLiteral(":/icons/home.png")),
      QString::fromUtf8("Главная"), sidebar);
  overview->setObjectName("navActive");
  overview->setMinimumHeight(54);
  overview->setIconSize(QSize(30, 30));
  overview->setCursor(Qt::PointingHandCursor);
  auto* projects = new QPushButton(
      QIcon(QStringLiteral(":/icons/projects.png")),
      QString::fromUtf8("Проекты"), sidebar);
  projects->setObjectName("navButton");
  projects->setMinimumHeight(48);
  projects->setIconSize(QSize(30, 30));
  projects->setCursor(Qt::PointingHandCursor);
  auto* settings = new QPushButton(
      QIcon(QStringLiteral(":/icons/settings.png")),
      QString::fromUtf8("Настройки"), sidebar);
  settings->setObjectName("navButton");
  settings->setMinimumHeight(48);
  settings->setIconSize(QSize(30, 30));
  settings->setCursor(Qt::PointingHandCursor);
  sidebarLayout->addWidget(overview);
  sidebarLayout->addWidget(projects);
  sidebarLayout->addWidget(settings);
  sidebarLayout->addStretch();

  auto* version = new QLabel(QString::fromUtf8("Открытая параметрическая САПР\nВерсия 0.1.0"), sidebar);
  version->setObjectName("versionLabel");
  sidebarLayout->addWidget(version);

  auto* content = new QWidget(root);
  content->setObjectName("content");
  auto* contentLayout = new QVBoxLayout(content);
  contentLayout->setContentsMargins(54, 44, 54, 48);
  contentLayout->setSpacing(28);

  auto* headingRow = new QHBoxLayout;
  auto* headingColumn = new QVBoxLayout;
  auto* heading = new QLabel(QString::fromUtf8("Добро пожаловать!"), content);
  heading->setObjectName("heading");
  auto* subtitle = new QLabel(
      QString::fromUtf8("Начните новый инженерный проект или продолжите работу с существующим."),
      content);
  subtitle->setObjectName("subtitle");
  headingColumn->addWidget(heading);
  headingColumn->addWidget(subtitle);
  headingRow->addLayout(headingColumn);
  headingRow->addStretch();
  contentLayout->addLayout(headingRow);

  auto* sectionTitle = new QLabel(QString::fromUtf8("Проекты"), content);
  sectionTitle->setObjectName("sectionTitle");
  contentLayout->addWidget(sectionTitle);

  QPushButton* createButton = nullptr;
  QPushButton* openButton = nullptr;
  auto* cards = new QHBoxLayout;
  cards->setSpacing(24);
  cards->addWidget(makeActionCard(
      "+", QString::fromUtf8("Создать проект"),
      QString::fromUtf8("Создайте новый файл проекта и откройте рабочее пространство САПР."),
      QString::fromUtf8("＋  Создать проект"), true, content, &createButton));
  cards->addWidget(makeActionCard(
      "↗", QString::fromUtf8("Открыть проект"),
      QString::fromUtf8("Выберите сохранённый проект Солидарность CAD на этом компьютере."),
      QString::fromUtf8("Открыть проект"), false, content, &openButton));
  contentLayout->addLayout(cards);
  contentLayout->addStretch();

  connect(createButton, &QPushButton::clicked, this, &HomeWindow::createProject);
  connect(openButton, &QPushButton::clicked, this, &HomeWindow::openProject);
  connect(projects, &QPushButton::clicked, this, &HomeWindow::openProject);

  rootLayout->addWidget(sidebar);
  rootLayout->addWidget(content, 1);
  setCentralWidget(root);

  setStyleSheet(R"(
    QWidget#root, QWidget#content { background: #f8faff; }
    QFrame#sidebar { background: #ffffff; border-right: 1px solid #dce5f3; }
    QPushButton#navActive { background: #086cff; color: white; border: none;
      border-radius: 10px; text-align: left; padding-left: 22px; font-size: 15px; }
    QPushButton#navButton { background: transparent; color: #254477; border: none;
      border-radius: 10px; text-align: left; padding-left: 22px; font-size: 15px; }
    QPushButton#navButton:hover { background: #edf5ff; }
    QLabel#versionLabel { color: #7c8eaa; font-size: 12px; line-height: 1.4; }
    QLabel#heading { color: #102c69; font-size: 34px; font-weight: 700; }
    QLabel#subtitle { color: #667a9e; font-size: 15px; }
    QLabel#sectionTitle { color: #17346f; font-size: 20px; font-weight: 650; }
    QFrame#actionCard { background: white; border: 1px solid #dce5f3;
      border-radius: 14px; }
    QFrame#actionCard:hover { border: 1px solid #9fc5ff; }
    QLabel#cardIcon { background: #eaf4ff; color: #0872ff; border-radius: 29px;
      font-size: 30px; font-weight: 500; }
    QLabel#cardTitle { color: #102c69; font-size: 22px; font-weight: 700; }
    QLabel#cardDescription { color: #6d7f9e; font-size: 14px; }
    QPushButton#primaryButton { background: #086cff; color: white; border: none;
      border-radius: 9px; font-size: 15px; font-weight: 600; }
    QPushButton#primaryButton:hover { background: #0059dc; }
    QPushButton#secondaryButton { background: white; color: #0864df;
      border: 1px solid #86b8ff; border-radius: 9px; font-size: 15px; font-weight: 600; }
    QPushButton#secondaryButton:hover { background: #edf5ff; }
  )");
}

void HomeWindow::createProject() {
  QString path = QFileDialog::getSaveFileName(
      this, QString::fromUtf8("Создать проект"),
      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
          "/Новый проект.solidar",
      QString::fromUtf8("Проект Солидарность CAD (*.solidar)"));
  if (path.isEmpty()) return;
  if (!path.endsWith(".solidar", Qt::CaseInsensitive)) path += ".solidar";
  QString error;
  if (!project::ProjectFile::create(path, &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Не удалось создать проект"), error);
    return;
  }
  emit projectRequested(path);
}

void HomeWindow::openProject() {
  const QString path = QFileDialog::getOpenFileName(
      this, QString::fromUtf8("Открыть проект"),
      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
      QString::fromUtf8("Проект Солидарность CAD (*.solidar)"));
  if (path.isEmpty()) return;
  QString error;
  if (!project::ProjectFile::validate(path, &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Не удалось открыть проект"), error);
    return;
  }
  emit projectRequested(path);
}

}  // namespace solidar::home
