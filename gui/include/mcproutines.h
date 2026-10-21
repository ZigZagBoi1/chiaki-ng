// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_MCPROUTINES_H
#define CHIAKI_MCPROUTINES_H

#include "mcpserver.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSharedPointer>
#include <QStringList>

#include <functional>

class McpUiTools;

/**
 * Saved, replayable automation for MCP clients.
 *
 * A routine is a named list of steps (controller input, keys, waits, checks, other routines)
 * worked out once by an agent and then replayed at input speed, without the agent having to
 * look at every screen. Checkpoints are reference pictures of known screens that routines and
 * agents use to recognize where they are without a screenshot. Both persist across restarts.
 */
class McpRoutines : public QObject
{
	Q_OBJECT

	public:
		McpRoutines(McpServer *server, McpUiTools *ui_tools, QObject *parent = nullptr);

		void RegisterTools();
		QString StoragePath() const;

	private:
		struct RunState
		{
			QString name;
			QStringList log;
			QElapsedTimer elapsed;
			bool cancelled = false;
			int steps_run = 0;
		};
		using Done = std::function<void(bool ok, const QString &failure)>;

		void Load();
		bool Save(QString *error);

		bool ValidateSteps(const QJsonArray &steps, const QString &routine_name, QString *error) const;
		void CallTool(const QString &name, const QJsonObject &args, const std::function<void(const QJsonObject &result)> &callback);
		void RunRoutine(const QSharedPointer<RunState> &run, const QString &name, int depth, const Done &done);
		void RunAttempt(const QSharedPointer<RunState> &run, const QString &name, int depth, int attempt, int retries, const Done &done);
		void RunSteps(const QSharedPointer<RunState> &run, const QJsonArray &steps, int index, int depth, const Done &done);
		void RunStep(const QSharedPointer<RunState> &run, const QJsonObject &step, int depth, const Done &done);
		void WaitForCheckpoint(const QString &name, int timeout_ms, double min_similarity, const Done &done);
		QJsonArray MatchCheckpoints(const QImage &thumbnail) const;

		QPointer<McpServer> server;
		QPointer<McpUiTools> ui_tools;
		QJsonObject routines;    // name -> {description, steps, retries, recovery, updated}
		QJsonObject checkpoints; // name -> {description, thumbnail (base64 PNG), updated}
		QSharedPointer<RunState> current;
};

#endif // CHIAKI_MCPROUTINES_H
