/*
 * Dump Delay for OBS Studio
 * Copyright (C) 2026 Valerio Enrico Salvo (Ves)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version. See LICENSE.
 */

/* The dock (status + PANICONE button) and the settings window. */

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "dump-delay.h"
#include "plugin-support.h"

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QPixmap moduleImage(const char *file)
{
	QPixmap image;
	char *path = obs_module_file(file);
	if (path) {
		image = QPixmap(QString::fromUtf8(path));
		bfree(path);
	}
	return image;
}

bool sceneExists(const QString &name)
{
	obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
	bool exists = source && obs_scene_from_source(source);
	obs_source_release(source);
	return exists;
}

/* ------------------------------------------------------------------------ */

/* The PANICONE image as a button filling whatever space the dock gives it:
 * greyed out when disabled, slightly smaller while held down. */
class PanicButton : public QAbstractButton {
public:
	explicit PanicButton(QWidget *parent) : QAbstractButton(parent), image(moduleImage("panicone.png"))
	{
		if (!image.isNull())
			disabledImage = QIcon(image).pixmap(image.size(), QIcon::Disabled);

		setCursor(Qt::PointingHandCursor);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	}

	QSize sizeHint() const override { return {256, 272}; }
	QSize minimumSizeHint() const override { return {96, 96}; }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::SmoothPixmapTransform);

		if (image.isNull()) {
			painter.drawText(rect(), Qt::AlignCenter, "PANIC");
			return;
		}

		QSize size = image.size().scaled(this->size(), Qt::KeepAspectRatio);
		if (isDown())
			size *= 0.94;
		QRect target(QPoint(0, 0), size);
		target.moveCenter(rect().center());
		painter.drawPixmap(target, isEnabled() ? image : disabledImage);
	}

private:
	QPixmap image;
	QPixmap disabledImage;
};

/* ------------------------------------------------------------------------ */

class SettingsDialog : public QDialog {
public:
	explicit SettingsDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(T("Settings.Title"));
		setAttribute(Qt::WA_DeleteOnClose);
		resize(520, 560);

		auto *tabs = new QTabWidget(this);
		tabs->addTab(createSettingsTab(), T("Settings.Tab.Settings"));
		tabs->addTab(createTextTab(T("Help.Text")), T("Settings.Tab.Help"));
		tabs->addTab(createAboutTab(), T("Settings.Tab.About"));

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
		buttons->button(QDialogButtonBox::Close)->setText(T("Settings.Close"));
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

		auto *layout = new QVBoxLayout(this);
		layout->addWidget(tabs);
		layout->addWidget(buttons);

		loadConfig();
		refreshScenes();
		refreshState();

		auto *timer = new QTimer(this);
		connect(timer, &QTimer::timeout, [this] { refreshState(); });
		timer->start(500);

		obs_frontend_add_event_callback(onFrontendEvent, this);
	}

	~SettingsDialog() override { obs_frontend_remove_event_callback(onFrontendEvent, this); }

private:
	QWidget *createSettingsTab()
	{
		auto *page = new QWidget;

		enable = new QCheckBox(T("Settings.Enable"), page);
		enable->setToolTip(T("Settings.Enable.Tooltip"));

		delay = new QSpinBox(page);
		delay->setRange(3, 60);
		delay->setSuffix(" s");
		delay->setToolTip(T("Settings.Delay.Tooltip"));

		slate = new QComboBox(page);
		slate->setToolTip(T("Settings.Slate.Tooltip"));
		slate->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

		createSlate = new QPushButton(T("Settings.Create"), page);
		createSlate->setToolTip(T("Settings.Create.Tooltip"));

		auto *slateRow = new QHBoxLayout;
		slateRow->setContentsMargins(0, 0, 0, 0);
		slateRow->addWidget(slate);
		slateRow->addWidget(createSlate);

		margin = new QSpinBox(page);
		margin->setRange(0, 5000);
		margin->setSingleStep(100);
		margin->setSuffix(" ms");
		margin->setToolTip(T("Settings.Margin.Tooltip"));

		auto *delayLabel = new QLabel(T("Settings.Delay"), page);
		delayLabel->setToolTip(delay->toolTip());
		auto *slateLabel = new QLabel(T("Settings.Slate"), page);
		slateLabel->setToolTip(slate->toolTip());
		auto *marginLabel = new QLabel(T("Settings.Margin"), page);
		marginLabel->setToolTip(margin->toolTip());

		locked = new QLabel(T("Settings.Locked"), page);
		locked->setWordWrap(true);
		locked->setStyleSheet("color: #d29922;");

		message = new QLabel(page);
		message->setWordWrap(true);
		message->setStyleSheet("color: #e5534b;");

		auto *form = new QFormLayout;
		form->addRow(enable);
		form->addRow(delayLabel, delay);
		form->addRow(slateLabel, slateRow);
		form->addRow(marginLabel, margin);

		auto *layout = new QVBoxLayout(page);
		layout->addLayout(form);
		layout->addWidget(locked);
		layout->addWidget(message);
		layout->addStretch(1);

		connect(enable, &QCheckBox::toggled, [this](bool on) { setServiceEnabled(on); });
		connect(delay, &QSpinBox::valueChanged, [this] { saveConfig(); });
		connect(margin, &QSpinBox::valueChanged, [this] { saveConfig(); });
		connect(slate, &QComboBox::currentIndexChanged, [this] { saveConfig(); });
		connect(createSlate, &QPushButton::clicked,
			[this] { dump_delay_create_slate_scene(slateName.toUtf8().constData()); });
		return page;
	}

	QWidget *createTextTab(const QString &html)
	{
		auto *label = new QLabel(html);
		label->setWordWrap(true);
		label->setTextFormat(Qt::RichText);
		label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
		label->setMargin(8);

		auto *scroll = new QScrollArea;
		scroll->setWidget(label);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		return scroll;
	}

	QWidget *createAboutTab()
	{
		auto *page = new QWidget;

		auto *logo = new QLabel(page);
		QPixmap image = moduleImage("panicone.png");
		if (!image.isNull())
			logo->setPixmap(image.scaledToWidth(128, Qt::SmoothTransformation));
		logo->setAlignment(Qt::AlignCenter);

		auto *text = new QLabel(T("About.Text").arg(QString::fromUtf8(PLUGIN_VERSION)), page);
		text->setWordWrap(true);
		text->setTextFormat(Qt::RichText);
		text->setAlignment(Qt::AlignCenter);

		/* thanks, with the channel logo linking to the channel */
		auto *thanksLogo = new QPushButton(page);
		QPixmap locanda = moduleImage("locanda.png");
		thanksLogo->setIcon(QIcon(locanda));
		thanksLogo->setIconSize({72, 72});
		thanksLogo->setFlat(true);
		thanksLogo->setCursor(Qt::PointingHandCursor);
		thanksLogo->setToolTip(T("About.ThanksUrl"));
		thanksLogo->setStyleSheet("QPushButton { border: none; background: transparent; }");
		thanksLogo->setVisible(!locanda.isNull());
		connect(thanksLogo, &QPushButton::clicked,
			[] { QDesktopServices::openUrl(QUrl(T("About.ThanksUrl"))); });

		auto *thanks = new QLabel(T("About.Thanks").arg(T("About.ThanksUrl")), page);
		thanks->setWordWrap(true);
		thanks->setTextFormat(Qt::RichText);
		thanks->setAlignment(Qt::AlignCenter);
		thanks->setOpenExternalLinks(true);

		auto *layout = new QVBoxLayout(page);
		layout->addStretch(1);
		layout->addWidget(logo);
		layout->addWidget(text);
		layout->addSpacing(12);
		layout->addWidget(thanksLogo, 0, Qt::AlignHCenter);
		layout->addWidget(thanks);
		layout->addStretch(1);
		return page;
	}

	static void onFrontendEvent(enum obs_frontend_event event, void *data)
	{
		if (event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED ||
		    event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
			static_cast<SettingsDialog *>(data)->refreshScenes();
	}

	void loadConfig()
	{
		dump_delay_config cfg;
		dump_delay_config_get(&cfg);

		QSignalBlocker b1(delay), b2(margin);
		delay->setValue(cfg.delay_sec);
		margin->setValue(cfg.cut_margin_ms);
		slateName = QString::fromUtf8(cfg.slate_scene);
	}

	void saveConfig()
	{
		if (slate->currentIndex() >= 0)
			slateName = slate->currentData().toString();

		dump_delay_config cfg = {};
		cfg.delay_sec = delay->value();
		snprintf(cfg.slate_scene, sizeof(cfg.slate_scene), "%s", slateName.toUtf8().constData());
		cfg.cut_margin_ms = margin->value();
		dump_delay_config_set(&cfg);
		refreshScenes();
	}

	/* The slate list is OBS's scenes; a configured slate that doesn't
	 * exist stays selectable but is flagged, with a Create button. */
	void refreshScenes()
	{
		QSignalBlocker blocker(slate);
		slate->clear();

		char **names = obs_frontend_get_scene_names();
		for (char **name = names; name && *name; name++) {
			QString scene = QString::fromUtf8(*name);
			slate->addItem(scene, scene);
		}
		bfree(names);

		bool missing = !slateName.isEmpty() && slate->findData(slateName) < 0;
		if (missing)
			slate->addItem(T("Settings.Slate.Missing").arg(slateName), slateName);
		slate->setCurrentIndex(slate->findData(slateName));
		createSlate->setVisible(missing);
		updateMessage();
	}

	void setServiceEnabled(bool on)
	{
		const char *error = nullptr;
		bool ok = on ? dump_delay_enable(&error) : dump_delay_disable(&error);

		serviceError = ok ? QString() : QString::fromUtf8(error);
		if (!ok) {
			QSignalBlocker blocker(enable);
			enable->setChecked(!on);
		}
		updateMessage();
	}

	void updateMessage()
	{
		QString text = serviceError;
		if (text.isEmpty() && dump_delay_obs_delay_enabled())
			text = T("Warn.ObsDelay");
		if (text.isEmpty() && !sceneExists(slateName))
			text = T("Warn.SlateMissing").arg(slateName);
		message->setText(text);
		message->setVisible(!text.isEmpty());
	}

	/* settings are read when the stream starts */
	void refreshState()
	{
		bool streaming = obs_frontend_streaming_active();
		bool enabled = dump_delay_is_enabled();

		if (enable->isChecked() != enabled) {
			QSignalBlocker blocker(enable);
			enable->setChecked(enabled);
		}
		for (QWidget *w : {static_cast<QWidget *>(enable), static_cast<QWidget *>(delay),
				   static_cast<QWidget *>(slate), static_cast<QWidget *>(createSlate),
				   static_cast<QWidget *>(margin)})
			w->setEnabled(!streaming);
		locked->setVisible(streaming);
		updateMessage(); /* OBS settings may have changed meanwhile */
	}

	QCheckBox *enable;
	QSpinBox *delay;
	QComboBox *slate;
	QPushButton *createSlate;
	QSpinBox *margin;
	QLabel *locked;
	QLabel *message;

	QString slateName;
	QString serviceError;
};

QPointer<SettingsDialog> settingsDialog;

/* ------------------------------------------------------------------------ */

class DumpDelayDock : public QWidget {
public:
	explicit DumpDelayDock(QWidget *parent) : QWidget(parent)
	{
		status = new QLabel(this);
		status->setAlignment(Qt::AlignCenter);
		status->setWordWrap(true);
		status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
		status->setToolTip(T("Status.Tooltip"));

		panic = new PanicButton(this);

		auto *hint = new QLabel(T("Dock.HotkeyHint"), this);
		hint->setAlignment(Qt::AlignCenter);
		hint->setWordWrap(true);
		hint->setStyleSheet("color: gray; font-size: small;");
		hint->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

		warning = new QLabel(this);
		warning->setAlignment(Qt::AlignCenter);
		warning->setWordWrap(true);
		warning->setStyleSheet("color: #e5534b;");
		warning->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

		auto *settings = new QPushButton(T("Dock.Settings"), this);
		settings->setToolTip(T("Dock.SettingsTooltip"));

		auto *layout = new QVBoxLayout(this);
		layout->addWidget(status);
		layout->addWidget(panic, 1);
		layout->addWidget(hint);
		layout->addWidget(warning);
		layout->addWidget(settings);

		connect(panic, &QAbstractButton::clicked, [] { dump_delay_trigger(); });
		connect(settings, &QPushButton::clicked, [] { dump_delay_open_settings(); });

		refresh();
		auto *timer = new QTimer(this);
		connect(timer, &QTimer::timeout, [this] { refresh(); });
		timer->start(250);

		obs_frontend_add_event_callback(onFrontendEvent, this);
	}

	~DumpDelayDock() override { obs_frontend_remove_event_callback(onFrontendEvent, this); }

private:
	static void onFrontendEvent(enum obs_frontend_event event, void *data)
	{
		if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && dump_delay_config_take_first_run())
			static_cast<DumpDelayDock *>(data)->showDocked();
	}

	/* New docks start hidden; on first install put ours on the right. OBS
	 * remembers its place and visibility after that. */
	void showDocked()
	{
		auto *dockWidget = qobject_cast<QDockWidget *>(parentWidget());
		auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		if (!dockWidget || !mainWindow)
			return;

		mainWindow->addDockWidget(Qt::RightDockWidgetArea, dockWidget);
		dockWidget->setFloating(false);
		dockWidget->show();
	}

	void refresh()
	{
		dump_delay_config cfg;
		dump_delay_config_get(&cfg);

		dump_delay_status state = dump_delay_get_status();
		bool enabled = dump_delay_is_enabled();
		bool streaming = obs_frontend_streaming_active();

		bool canDump = state == DUMP_DELAY_LIVE || state == DUMP_DELAY_BUFFERING;
		panic->setEnabled(canDump);
		panic->setToolTip(canDump ? T("Panic.Tooltip") : T("Panic.DisabledTooltip"));

		QString text, color;
		switch (state) {
		case DUMP_DELAY_BUFFERING:
			text = T("Status.Buffering").arg(cfg.delay_sec);
			color = "#d29922";
			break;
		case DUMP_DELAY_LIVE:
			text = T("Status.Live").arg(cfg.delay_sec);
			color = "#3fb950";
			break;
		case DUMP_DELAY_DUMPING:
			text = T("Status.Dumping");
			color = "#e5534b";
			break;
		case DUMP_DELAY_OFF:
		default:
			if (!enabled)
				text = T("Status.Off");
			else if (streaming)
				text = T("Status.Connecting");
			else
				text = T("Status.Ready").arg(cfg.delay_sec);
			color = "gray";
			break;
		}
		if (text != status->text()) {
			status->setText(text);
			status->setStyleSheet(QString("font-weight: bold; color: %1;").arg(color));
		}

		QString warn;
		if (enabled && dump_delay_obs_delay_enabled())
			warn = T("Warn.ObsDelay");
		else if (!enabled)
			warn = T("Warn.Disabled");
		else if (!sceneExists(QString::fromUtf8(cfg.slate_scene)))
			warn = T("Warn.SlateMissing").arg(QString::fromUtf8(cfg.slate_scene));
		if (warn != warning->text())
			warning->setText(warn);
		warning->setVisible(!warn.isEmpty());
	}

	QLabel *status;
	PanicButton *panic;
	QLabel *warning;
};

} // namespace

extern "C" void dump_delay_dock_create(void)
{
	auto *dock = new DumpDelayDock(static_cast<QWidget *>(obs_frontend_get_main_window()));
	obs_frontend_add_dock_by_id("dump-delay-dock", obs_module_text("DumpDelay"), dock);
}

extern "C" void dump_delay_open_settings(void)
{
	if (!settingsDialog)
		settingsDialog = new SettingsDialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
	settingsDialog->show();
	settingsDialog->raise();
	settingsDialog->activateWindow();
}
