// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <mcproutines.h>
#include <mcpuitools.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace
{
constexpr int kMaxDepth = 4;
constexpr int kMaxRetries = 5;
constexpr int kMaxWaitMs = 120000;

const QStringList kStepKinds = {"controller", "key", "click_element", "wait_ms", "wait_for", "checkpoint", "routine", "ensure_stream", "tool"};
// Tools a routine must not call itself (recursion, destructive or meaningless in replay).
const QStringList kForbiddenTools = {"run_routine", "save_routine", "delete_routine", "stop_routine", "save_checkpoint", "delete_checkpoint"};

QJsonObject Schema(const QJsonObject &properties, const QStringList &required = {})
{
	QJsonObject schema{{"type", "object"}, {"properties", properties}, {"additionalProperties", false}};
	if(!required.isEmpty())
		schema["required"] = QJsonArray::fromStringList(required);
	return schema;
}

QJsonObject Prop(const QString &type, const QString &description)
{
	return QJsonObject{{"type", type}, {"description", description}};
}

QString ToJson(const QJsonObject &object)
{
	return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

QString FirstText(const QJsonObject &result)
{
	for(const QJsonValue &c : result.value("content").toArray())
		if(c.toObject().value("type").toString() == QLatin1String("text"))
			return c.toObject().value("text").toString();
	return QString();
}

QString StepKind(const QJsonObject &step)
{
	for(const QString &kind : kStepKinds)
		if(step.contains(kind))
			return kind;
	return QString();
}

QString StepLabel(const QJsonObject &step)
{
	if(step.contains("label"))
		return step.value("label").toString();
	const QString kind = StepKind(step);
	QString detail;
	const QJsonValue value = step.value(kind);
	if(value.isString())
		detail = value.toString();
	else if(value.isDouble())
		detail = QString::number(value.toInt());
	else if(value.isObject())
		detail = QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
	if(detail.size() > 70)
		detail = detail.left(67) + QStringLiteral("...");
	return QStringLiteral("%1 %2").arg(kind, detail);
}

QString EncodeThumbnail(const QImage &thumbnail)
{
	QByteArray data;
	QBuffer buffer(&data);
	buffer.open(QIODevice::WriteOnly);
	thumbnail.save(&buffer, "PNG");
	return QString::fromLatin1(data.toBase64());
}

QImage DecodeThumbnail(const QString &base64)
{
	QImage image;
	image.loadFromData(QByteArray::fromBase64(base64.toLatin1()), "PNG");
	return image.convertToFormat(QImage::Format_Grayscale8);
}
}

McpRoutines::McpRoutines(McpServer *server, McpUiTools *ui_tools, QObject *parent)
	: QObject(parent)
	, server(server)
	, ui_tools(ui_tools)
{
	Load();
}

QString McpRoutines::StoragePath() const
{
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/mcp-routines.json");
}

void McpRoutines::Load()
{
	QFile file(StoragePath());
	if(!file.open(QIODevice::ReadOnly))
	{
		// First run: starter routines that work on every PS5 regardless of layout.
		routines["ps5_go_home"] = QJsonObject{
			{"description", "Go to the PS5 home screen from anywhere (holds the PS button). Use as recovery routine."},
			{"steps", QJsonArray{
				QJsonObject{{"label", "hold PS"}, {"controller", QJsonObject{{"buttons", QJsonArray{"ps"}}, {"hold_ms", 1200}}}},
				QJsonObject{{"wait_ms", 1500}},
			}},
			{"retries", 0}, {"recovery", ""}, {"updated", QDateTime::currentDateTime().toString(Qt::ISODate)},
		};
		routines["ps5_control_center"] = QJsonObject{
			{"description", "Open the PS5 Control Center (short PS press)."},
			{"steps", QJsonArray{
				QJsonObject{{"label", "press PS"}, {"controller", QJsonObject{{"buttons", QJsonArray{"ps"}}, {"hold_ms", 150}}}},
				QJsonObject{{"wait_ms", 800}},
			}},
			{"retries", 0}, {"recovery", ""}, {"updated", QDateTime::currentDateTime().toString(Qt::ISODate)},
		};
		return;
	}
	const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
	routines = root.value("routines").toObject();
	checkpoints = root.value("checkpoints").toObject();
	qCInfo(chiakiMcp) << "Loaded" << routines.size() << "routines and" << checkpoints.size() << "checkpoints from" << StoragePath();
}

bool McpRoutines::Save(QString *error)
{
	QDir().mkpath(QFileInfo(StoragePath()).absolutePath());
	QSaveFile file(StoragePath());
	if(!file.open(QIODevice::WriteOnly))
	{
		*error = file.errorString();
		return false;
	}
	const QJsonObject root{{"version", 1}, {"routines", routines}, {"checkpoints", checkpoints}};
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	if(!file.commit())
	{
		*error = file.errorString();
		return false;
	}
	return true;
}

void McpRoutines::CallTool(const QString &name, const QJsonObject &args, const std::function<void(const QJsonObject &result)> &callback)
{
	if(!server)
	{
		callback(McpServer::ErrorResult(QStringLiteral("Server is gone")));
		return;
	}
	for(const McpServer::Tool &tool : server->Tools())
	{
		if(tool.name == name)
		{
			QString error;
			if(!McpServer::ValidateArguments(tool.input_schema, args, &error))
			{
				callback(McpServer::ErrorResult(QStringLiteral("Invalid arguments for %1: %2").arg(name, error)));
				return;
			}
			tool.handler(args, callback);
			return;
		}
	}
	callback(McpServer::ErrorResult(QStringLiteral("Unknown tool %1").arg(name)));
}

bool McpRoutines::ValidateSteps(const QJsonArray &steps, const QString &routine_name, QString *error) const
{
	if(steps.isEmpty())
	{
		*error = QStringLiteral("steps must not be empty");
		return false;
	}
	for(int i = 0; i < steps.size(); i++)
	{
		const QJsonObject step = steps[i].toObject();
		const QString kind = StepKind(step);
		if(kind.isEmpty())
		{
			*error = QStringLiteral("Step %1 has no known action. Use one of: %2").arg(i).arg(kStepKinds.join(QStringLiteral(", ")));
			return false;
		}
		if(kind == QLatin1String("checkpoint") && !checkpoints.contains(step.value("checkpoint").toString()))
		{
			*error = QStringLiteral("Step %1: checkpoint '%2' does not exist; create it with save_checkpoint while that screen is showing")
				.arg(i).arg(step.value("checkpoint").toString());
			return false;
		}
		if(kind == QLatin1String("routine"))
		{
			const QString ref = step.value("routine").toString();
			if(ref == routine_name)
			{
				*error = QStringLiteral("Step %1: a routine cannot call itself").arg(i);
				return false;
			}
			if(!routines.contains(ref))
			{
				*error = QStringLiteral("Step %1: routine '%2' does not exist yet").arg(i).arg(ref);
				return false;
			}
		}
		if(kind == QLatin1String("tool"))
		{
			const QString tool = step.value("tool").toObject().value("name").toString();
			if(tool.isEmpty() || kForbiddenTools.contains(tool))
			{
				*error = QStringLiteral("Step %1: tool step needs {name, args} and cannot call %2").arg(i).arg(kForbiddenTools.join(QStringLiteral(", ")));
				return false;
			}
		}
		// Check the arguments a step will pass, so a routine never saves something that cannot run.
		QString target_tool;
		QJsonObject target_args;
		if(kind == QLatin1String("controller"))
			target_tool = QStringLiteral("controller_input"), target_args = step.value("controller").toObject();
		else if(kind == QLatin1String("click_element"))
			target_tool = QStringLiteral("click_element"), target_args = step.value("click_element").toObject();
		else if(kind == QLatin1String("key") && step.value("key").isObject())
			target_tool = QStringLiteral("press_key"), target_args = step.value("key").toObject();
		else if(kind == QLatin1String("wait_for"))
			target_tool = QStringLiteral("wait_for"), target_args = step.value("wait_for").toObject();
		else if(kind == QLatin1String("tool"))
			target_tool = step.value("tool").toObject().value("name").toString(), target_args = step.value("tool").toObject().value("args").toObject();
		if(!target_tool.isEmpty() && server)
		{
			bool found = false;
			for(const McpServer::Tool &tool : server->Tools())
			{
				if(tool.name != target_tool)
					continue;
				found = true;
				QString arg_error;
				if(!McpServer::ValidateArguments(tool.input_schema, target_args, &arg_error))
				{
					*error = QStringLiteral("Step %1 (%2): %3").arg(i).arg(target_tool, arg_error);
					return false;
				}
			}
			if(!found)
			{
				*error = QStringLiteral("Step %1: unknown tool %2").arg(i).arg(target_tool);
				return false;
			}
		}
		if(kind == QLatin1String("wait_ms") && (step.value("wait_ms").toInt(-1) < 0 || step.value("wait_ms").toInt() > kMaxWaitMs))
		{
			*error = QStringLiteral("Step %1: wait_ms must be 0..%2").arg(i).arg(kMaxWaitMs);
			return false;
		}
	}
	return true;
}

QJsonArray McpRoutines::MatchCheckpoints(const QImage &thumbnail) const
{
	struct Match
	{
		QString name;
		double similarity;
	};
	QList<Match> matches;
	for(auto it = checkpoints.constBegin(); it != checkpoints.constEnd(); ++it)
	{
		const QImage reference = DecodeThumbnail(it.value().toObject().value("thumbnail").toString());
		matches.append({it.key(), McpUiTools::BlockSimilarity(reference, thumbnail)});
	}
	std::sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) { return a.similarity > b.similarity; });
	QJsonArray out;
	for(const Match &m : matches)
		out.append(QJsonObject{{"checkpoint", m.name}, {"similarity", qRound(m.similarity * 1000) / 1000.0},
				{"description", checkpoints.value(m.name).toObject().value("description")}});
	return out;
}

void McpRoutines::WaitForCheckpoint(const QString &name, int timeout_ms, double min_similarity, const Done &done)
{
	const QImage reference = DecodeThumbnail(checkpoints.value(name).toObject().value("thumbnail").toString());
	if(reference.isNull())
	{
		done(false, QStringLiteral("checkpoint '%1' is missing or corrupt").arg(name));
		return;
	}
	auto elapsed = QSharedPointer<QElapsedTimer>::create();
	elapsed->start();
	auto best = QSharedPointer<double>::create(0.0);
	auto poll = QSharedPointer<std::function<void()>>::create();
	QPointer<McpRoutines> self(this);
	*poll = [self, name, reference, timeout_ms, min_similarity, elapsed, best, poll, done]() {
		if(!self || !self->ui_tools)
			return;
		self->ui_tools->GrabComposite([self, name, reference, timeout_ms, min_similarity, elapsed, best, poll, done](const QImage &frame, const QString &error) {
			if(!self)
				return;
			auto finish = [poll, done](bool ok, const QString &failure) {
				// The closure references itself; release it after this event.
				QTimer::singleShot(0, QCoreApplication::instance(), [poll]() { *poll = nullptr; });
				done(ok, failure);
			};
			if(frame.isNull())
			{
				finish(false, QStringLiteral("could not capture the screen: %1").arg(error));
				return;
			}
			const double similarity = McpUiTools::BlockSimilarity(reference, McpUiTools::Thumbnail(frame));
			*best = qMax(*best, similarity);
			if(similarity >= min_similarity)
			{
				finish(true, QString());
				return;
			}
			if(elapsed->elapsed() >= timeout_ms)
			{
				finish(false, QStringLiteral("not on checkpoint '%1' (best similarity %2, needed %3)")
						.arg(name).arg(*best, 0, 'f', 2).arg(min_similarity, 0, 'f', 2));
				return;
			}
			QTimer::singleShot(300, self.data(), [poll]() { (*poll)(); });
		});
	};
	(*poll)();
}

void McpRoutines::RunStep(const QSharedPointer<RunState> &run, const QJsonObject &step, int depth, const Done &done)
{
	const QString kind = StepKind(step);
	QPointer<McpRoutines> self(this);
	auto tool_done = [done](const QJsonObject &result) {
		if(result.value("isError").toBool(false))
			done(false, FirstText(result).simplified().left(300));
		else
			done(true, QString());
	};

	if(kind == QLatin1String("controller"))
	{
		CallTool(QStringLiteral("controller_input"), step.value("controller").toObject(), tool_done);
	}
	else if(kind == QLatin1String("key"))
	{
		QJsonObject args = step.value("key").isObject() ? step.value("key").toObject() : QJsonObject{{"key", step.value("key")}};
		CallTool(QStringLiteral("press_key"), args, tool_done);
	}
	else if(kind == QLatin1String("click_element"))
	{
		CallTool(QStringLiteral("click_element"), step.value("click_element").toObject(), tool_done);
	}
	else if(kind == QLatin1String("tool"))
	{
		const QJsonObject tool = step.value("tool").toObject();
		CallTool(tool.value("name").toString(), tool.value("args").toObject(), tool_done);
	}
	else if(kind == QLatin1String("wait_ms"))
	{
		QTimer::singleShot(qBound(0, step.value("wait_ms").toInt(), kMaxWaitMs), this, [done]() { done(true, QString()); });
	}
	else if(kind == QLatin1String("wait_for"))
	{
		QJsonObject args = step.value("wait_for").toObject();
		args["screenshot"] = false;
		CallTool(QStringLiteral("wait_for"), args, [done](const QJsonObject &result) {
			const QJsonObject summary = QJsonDocument::fromJson(FirstText(result).toUtf8()).object();
			if(result.value("isError").toBool(false) || !summary.value("condition_met").toBool(false))
				done(false, summary.value("detail").toString(FirstText(result)).simplified().left(300));
			else
				done(true, QString());
		});
	}
	else if(kind == QLatin1String("checkpoint"))
	{
		WaitForCheckpoint(step.value("checkpoint").toString(), qBound(0, step.value("timeout_ms").toInt(5000), kMaxWaitMs),
				qBound(0.1, step.value("min_similarity").toDouble(0.8), 1.0), done);
	}
	else if(kind == QLatin1String("routine"))
	{
		RunRoutine(run, step.value("routine").toString(), depth + 1, done);
	}
	else if(kind == QLatin1String("ensure_stream"))
	{
		// Already streaming with a picture? Then nothing to do; otherwise connect and wait.
		const QJsonObject target = step.value("ensure_stream").toObject();
		CallTool(QStringLiteral("wait_for_stream"), QJsonObject{{"state", "video"}, {"timeout_ms", 0}, {"screenshot", false}},
				[self, target, done](const QJsonObject &result) {
			if(!self)
				return;
			if(QJsonDocument::fromJson(FirstText(result).toUtf8()).object().value("condition_met").toBool(false))
			{
				done(true, QString());
				return;
			}
			QJsonObject connect_args;
			if(target.contains("index"))
				connect_args["index"] = target.value("index");
			else
				connect_args["name"] = target.value("console");
			self->CallTool(QStringLiteral("connect_host"), connect_args, [self, target, done](const QJsonObject &connected) {
				if(!self)
					return;
				if(connected.value("isError").toBool(false))
				{
					done(false, FirstText(connected).simplified().left(300));
					return;
				}
				self->CallTool(QStringLiteral("wait_for_stream"),
						QJsonObject{{"state", "video"}, {"timeout_ms", qBound(1000, target.value("timeout_ms").toInt(60000), kMaxWaitMs)}, {"screenshot", false}},
						[done](const QJsonObject &waited) {
					if(QJsonDocument::fromJson(FirstText(waited).toUtf8()).object().value("condition_met").toBool(false))
						done(true, QString());
					else
						done(false, QStringLiteral("stream did not start"));
				});
			});
		});
	}
	else
	{
		done(false, QStringLiteral("unknown step"));
	}
}

void McpRoutines::RunSteps(const QSharedPointer<RunState> &run, const QJsonArray &steps, int index, int depth, const Done &done)
{
	if(run->cancelled)
	{
		done(false, QStringLiteral("stopped by stop_routine"));
		return;
	}
	if(index >= steps.size())
	{
		done(true, QString());
		return;
	}
	const QJsonObject step = steps[index].toObject();
	const QString indent(depth * 2, QLatin1Char(' '));
	const qint64 started = run->elapsed.elapsed();
	QPointer<McpRoutines> self(this);
	RunStep(run, step, depth, [self, run, steps, index, depth, done, step, indent, started](bool ok, const QString &failure) {
		if(!self)
			return;
		run->steps_run++;
		const bool optional = step.value("optional").toBool(false);
		const QString seconds = QString::number((run->elapsed.elapsed() - started) / 1000.0, 'f', 1);
		if(ok)
			run->log << QStringLiteral("%1ok   %2. %3 (%4 s)").arg(indent).arg(index + 1).arg(StepLabel(step), seconds);
		else
			run->log << QStringLiteral("%1%2 %3. %4 (%5 s): %6").arg(indent, optional ? QStringLiteral("skip") : QStringLiteral("FAIL"))
				.arg(index + 1).arg(StepLabel(step), seconds, failure);
		if(!ok && !optional)
		{
			done(false, QStringLiteral("step %1 (%2): %3").arg(index + 1).arg(StepLabel(step), failure));
			return;
		}
		self->RunSteps(run, steps, index + 1, depth, done);
	});
}

void McpRoutines::RunAttempt(const QSharedPointer<RunState> &run, const QString &name, int depth, int attempt, int retries, const Done &done)
{
	const QJsonObject routine = routines.value(name).toObject();
	const QString indent(depth * 2, QLatin1Char(' '));
	run->log << QStringLiteral("%1> %2%3").arg(indent, name, attempt > 0 ? QStringLiteral(" (retry %1)").arg(attempt) : QString());
	QPointer<McpRoutines> self(this);
	RunSteps(run, routine.value("steps").toArray(), 0, depth, [self, run, name, depth, attempt, retries, done, routine, indent](bool ok, const QString &failure) {
		if(!self)
			return;
		if(ok || attempt >= retries || run->cancelled)
		{
			done(ok, failure);
			return;
		}
		// Get back to a known state before trying again.
		const QString recovery = routine.value("recovery").toString();
		auto retry = [self, run, name, depth, attempt, retries, done]() {
			if(self)
				self->RunAttempt(run, name, depth, attempt + 1, retries, done);
		};
		self->CallTool(QStringLiteral("release_controller"), QJsonObject(), [self, run, recovery, depth, retry, indent](const QJsonObject &) {
			if(!self)
				return;
			if(recovery.isEmpty() || !self->routines.contains(recovery))
			{
				retry();
				return;
			}
			run->log << QStringLiteral("%1  recovering with %2").arg(indent, recovery);
			self->RunRoutine(run, recovery, depth + 1, [retry](bool, const QString &) { retry(); });
		});
	});
}

void McpRoutines::RunRoutine(const QSharedPointer<RunState> &run, const QString &name, int depth, const Done &done)
{
	if(depth > kMaxDepth)
	{
		done(false, QStringLiteral("routines nested deeper than %1").arg(kMaxDepth));
		return;
	}
	if(!routines.contains(name))
	{
		done(false, QStringLiteral("routine '%1' does not exist").arg(name));
		return;
	}
	const int retries = qBound(0, routines.value(name).toObject().value("retries").toInt(0), kMaxRetries);
	RunAttempt(run, name, depth, 0, retries, done);
}

void McpRoutines::RegisterTools()
{
	QPointer<McpRoutines> self(this);
	const QJsonObject step_schema{
		{"type", "object"},
		{"description",
			"Exactly one action: controller {controller_input args}, key \"Return\" | {press_key args}, "
			"click_element {args}, wait_ms 1000, wait_for {until, text, ...}, checkpoint \"name\" (+timeout_ms, "
			"min_similarity 0.8), routine \"other_routine\", ensure_stream {console | index}, tool {name, args}. "
			"Optional: label (shown in the log), optional true (a failure does not stop the routine)."},
	};

	server->AddTool({
		"save_routine",
		"Save routine",
		"Save (or replace) a reusable routine: the exact steps that accomplished a task, so later runs replay them "
		"at input speed without looking at every screen. Build it from steps you just verified by hand. Put a "
		"checkpoint or wait_for after each important transition so a replay fails fast instead of drifting.",
		Schema({
			{"name", Prop("string", "Routine name, e.g. relaunch_youtube")},
			{"description", Prop("string", "What it does and from which screen it starts")},
			{"steps", QJsonObject{{"type", "array"}, {"items", step_schema}, {"description", "Steps in order"}}},
			{"retries", Prop("integer", "Retry the whole routine this many times on failure (default 0, max 5)")},
			{"recovery", Prop("string", "Routine run before each retry to reach a known state, e.g. ps5_go_home")},
		}, {"name", "steps"}),
		false,
		[self](const QJsonObject &args, const McpServer::Responder &respond) {
			if(!self)
				return;
			const QString name = args.value("name").toString().trimmed();
			if(name.isEmpty())
			{
				respond(McpServer::ErrorResult(QStringLiteral("name is required")));
				return;
			}
			QString error;
			if(!self->ValidateSteps(args.value("steps").toArray(), name, &error))
			{
				respond(McpServer::ErrorResult(error));
				return;
			}
			const QString recovery = args.value("recovery").toString();
			if(!recovery.isEmpty() && (recovery == name || !self->routines.contains(recovery)))
			{
				respond(McpServer::ErrorResult(QStringLiteral("recovery must name another existing routine")));
				return;
			}
			const bool replaced = self->routines.contains(name);
			self->routines[name] = QJsonObject{
				{"description", args.value("description").toString()},
				{"steps", args.value("steps").toArray()},
				{"retries", qBound(0, args.value("retries").toInt(0), kMaxRetries)},
				{"recovery", recovery},
				{"updated", QDateTime::currentDateTime().toString(Qt::ISODate)},
			};
			if(!self->Save(&error))
			{
				respond(McpServer::ErrorResult(QStringLiteral("Could not write %1: %2").arg(self->StoragePath(), error)));
				return;
			}
			respond(McpServer::TextResult(QStringLiteral("%1 routine '%2' with %3 steps. Test it now with run_routine.")
					.arg(replaced ? QStringLiteral("Replaced") : QStringLiteral("Saved"), name).arg(args.value("steps").toArray().size())));
		},
	});

	server->AddTool({
		"run_routine",
		"Run routine",
		"Replay a saved routine. Returns a compact step log; only on failure it adds a screenshot and which step "
		"failed, so successful runs cost almost no tokens. Prefer this over manual navigation for any task done "
		"before (list_routines). After a failure: look at the screenshot, fix the routine with save_routine.",
		Schema({
			{"name", Prop("string", "Routine to run")},
			{"screenshot_on_failure", Prop("boolean", "Attach a screenshot when it fails (default true)")},
		}, {"name"}),
		false,
		[self](const QJsonObject &args, const McpServer::Responder &respond) {
			if(!self)
				return;
			const QString name = args.value("name").toString();
			if(!self->routines.contains(name))
			{
				QStringList names = self->routines.keys();
				respond(McpServer::ErrorResult(QStringLiteral("No routine '%1'. Saved routines: %2").arg(name,
						names.isEmpty() ? QStringLiteral("none yet (save_routine)") : names.join(QStringLiteral(", ")))));
				return;
			}
			if(self->current)
			{
				respond(McpServer::ErrorResult(QStringLiteral("Routine '%1' is still running; wait or call stop_routine").arg(self->current->name)));
				return;
			}
			auto run = QSharedPointer<RunState>::create();
			run->name = name;
			run->elapsed.start();
			self->current = run;
			const bool screenshot = args.value("screenshot_on_failure").toBool(true);
			self->RunRoutine(run, name, 0, [self, run, name, screenshot, respond](bool ok, const QString &failure) {
				if(!self)
					return;
				if(self->current == run)
					self->current.reset();
				QJsonObject summary{
					{"routine", name},
					{"ok", ok},
					{"elapsed_ms", qint64(run->elapsed.elapsed())},
					{"steps_run", run->steps_run},
					{"log", QJsonArray::fromStringList(run->log)},
				};
				if(!ok)
					summary["failure"] = failure;
				QJsonObject text = McpServer::TextResult(ToJson(summary));
				if(ok || !screenshot || !self->ui_tools)
				{
					if(!ok)
						text["isError"] = true;
					respond(text);
					return;
				}
				self->CallTool(QStringLiteral("release_controller"), QJsonObject(), [self, text, respond](const QJsonObject &) {
					if(!self || !self->ui_tools)
						return;
					self->ui_tools->TakeScreenshot(QJsonObject{{"max_width", 960}, {"restore_if_minimized", false}}, [text, respond](const QJsonObject &shot) {
						QJsonArray content = text.value("content").toArray();
						if(!shot.value("isError").toBool(false))
							for(const QJsonValue &c : shot.value("content").toArray())
								content.append(c);
						respond(QJsonObject{{"content", content}, {"isError", true}});
					});
				});
			});
		},
	});

	server->AddTool({
		"stop_routine",
		"Stop routine",
		"Stop the running routine after its current step and release all virtual controller input.",
		Schema({}),
		false,
		[self](const QJsonObject &, const McpServer::Responder &respond) {
			if(!self)
				return;
			if(!self->current)
			{
				respond(McpServer::TextResult(QStringLiteral("No routine is running")));
				return;
			}
			self->current->cancelled = true;
			self->CallTool(QStringLiteral("release_controller"), QJsonObject(), [respond](const QJsonObject &) {
				respond(McpServer::TextResult(QStringLiteral("Stopping after the current step")));
			});
		},
	});

	server->AddTool({
		"list_routines",
		"List routines",
		"Saved routines (name, description, steps, retries, recovery) and checkpoints. Check this before doing a "
		"task by hand: if a routine exists, run_routine is far faster and cheaper.",
		Schema({}),
		true,
		[self](const QJsonObject &, const McpServer::Responder &respond) {
			if(!self)
				return;
			QJsonArray routine_list, checkpoint_list;
			for(auto it = self->routines.constBegin(); it != self->routines.constEnd(); ++it)
			{
				const QJsonObject r = it.value().toObject();
				routine_list.append(QJsonObject{
					{"name", it.key()}, {"description", r.value("description")}, {"steps", r.value("steps").toArray().size()},
					{"retries", r.value("retries")}, {"recovery", r.value("recovery")}, {"updated", r.value("updated")},
				});
			}
			for(auto it = self->checkpoints.constBegin(); it != self->checkpoints.constEnd(); ++it)
				checkpoint_list.append(QJsonObject{{"name", it.key()}, {"description", it.value().toObject().value("description")}});
			respond(McpServer::TextResult(ToJson(QJsonObject{{"routines", routine_list}, {"checkpoints", checkpoint_list}, {"file", self->StoragePath()}})));
		},
	});

	server->AddTool({
		"get_routine",
		"Get routine",
		"The full definition of a saved routine (to inspect or fix it).",
		Schema({{"name", Prop("string", "Routine name")}}, {"name"}),
		true,
		[self](const QJsonObject &args, const McpServer::Responder &respond) {
			if(!self)
				return;
			const QString name = args.value("name").toString();
			if(!self->routines.contains(name))
			{
				respond(McpServer::ErrorResult(QStringLiteral("No routine '%1'").arg(name)));
				return;
			}
			QJsonObject r = self->routines.value(name).toObject();
			r["name"] = name;
			respond(McpServer::TextResult(ToJson(r)));
		},
	});

	server->AddTool({
		"delete_routine",
		"Delete routine",
		"Delete a saved routine.",
		Schema({{"name", Prop("string", "Routine name")}}, {"name"}),
		false,
		[self](const QJsonObject &args, const McpServer::Responder &respond) {
			if(!self)
				return;
			const QString name = args.value("name").toString();
			if(!self->routines.contains(name))
			{
				respond(McpServer::ErrorResult(QStringLiteral("No routine '%1'").arg(name)));
				return;
			}
			QStringList users;
			for(auto it = self->routines.constBegin(); it != self->routines.constEnd(); ++it)
			{
				const QJsonObject r = it.value().toObject();
				bool uses = r.value("recovery").toString() == name;
				for(const QJsonValue &s : r.value("steps").toArray())
					uses = uses || s.toObject().value("routine").toString() == name;
				if(uses && it.key() != name)
					users << it.key();
			}
			if(!users.isEmpty())
			{
				respond(McpServer::ErrorResult(QStringLiteral("'%1' is used by %2; update those first").arg(name, users.join(QStringLiteral(", ")))));
				return;
			}
			self->routines.remove(name);
			QString error;
			if(!self->Save(&error))
			{
				respond(McpServer::ErrorResult(error));
				return;
			}
			respond(McpServer::TextResult(QStringLiteral("Deleted routine '%1'").arg(name)));
		},
	});

	server->AddTool({
		"save_checkpoint",
		"Save checkpoint",
		"Remember the screen showing right now as a named checkpoint (e.g. ps5_home, youtube_home). Routines use "
		"checkpoints to confirm where they are, and match_checkpoints recognizes the current screen without a "
		"screenshot. Save it while the screen is settled (no popup, no transition).",
		Schema({
			{"name", Prop("string", "Checkpoint name")},
			{"description", Prop("string", "What screen this is")},
		}, {"name"}),
		false,
		[self](const QJsonObject &args, const McpServer::Responder &respond) {
			if(!self || !self->ui_tools)
				return;
			const QString name = args.value("name").toString().trimmed();
			if(name.isEmpty())
			{
				respond(McpServer::ErrorResult(QStringLiteral("name is required")));
				return;
			}
			self->ui_tools->GrabComposite([self, name, args, respond](const QImage &frame, const QString &error) {
				if(!self)
					return;
				if(frame.isNull())
				{
					respond(McpServer::ErrorResult(QStringLiteral("Could not capture the screen: %1").arg(error)));
					return;
				}
				self->checkpoints[name] = QJsonObject{
					{"description", args.value("description").toString()},
					{"thumbnail", EncodeThumbnail(McpUiTools::Thumbnail(frame))},
					{"updated", QDateTime::currentDateTime().toString(Qt::ISODate)},
				};
				QString save_error;
				if(!self->Save(&save_error))
				{
					respond(McpServer::ErrorResult(save_error));
					return;
				}
				respond(McpServer::TextResult(QStringLiteral("Saved checkpoint '%1'").arg(name)));
			});
		},
	});

	server->AddTool({
		"match_checkpoints",
		"Match checkpoints",
		"Compare the current screen with every saved checkpoint and return them ranked by similarity (0-1, a "
		"match is usually >= 0.8). A cheap way to know \"where am I\" on the console without a screenshot.",
		Schema({}),
		true,
		[self](const QJsonObject &, const McpServer::Responder &respond) {
			if(!self || !self->ui_tools)
				return;
			if(self->checkpoints.isEmpty())
			{
				respond(McpServer::ErrorResult(QStringLiteral("No checkpoints saved yet; use save_checkpoint on known screens first")));
				return;
			}
			self->ui_tools->GrabComposite([self, respond](const QImage &frame, const QString &error) {
				if(!self)
					return;
				if(frame.isNull())
				{
					respond(McpServer::ErrorResult(QStringLiteral("Could not capture the screen: %1").arg(error)));
					return;
				}
				const QJsonArray matches = self->MatchCheckpoints(McpUiTools::Thumbnail(frame));
				QJsonObject result{{"matches", matches}};
				const QJsonObject best = matches.first().toObject();
				result["best"] = best.value("similarity").toDouble() >= 0.8 ? best.value("checkpoint") : QJsonValue(QStringLiteral("none (unknown screen)"));
				respond(McpServer::TextResult(ToJson(result)));
			});
		},
	});

	server->AddTool({
		"delete_checkpoint",
		"Delete checkpoint",
		"Delete a saved checkpoint (fails if a routine still uses it).",
		Schema({{"name", Prop("string", "Checkpoint name")}}, {"name"}),
		false,
		[self](const QJsonObject &args, const McpServer::Responder &respond) {
			if(!self)
				return;
			const QString name = args.value("name").toString();
			QStringList users;
			for(auto it = self->routines.constBegin(); it != self->routines.constEnd(); ++it)
				for(const QJsonValue &s : it.value().toObject().value("steps").toArray())
					if(s.toObject().value("checkpoint").toString() == name && !users.contains(it.key()))
						users << it.key();
			if(!users.isEmpty())
			{
				respond(McpServer::ErrorResult(QStringLiteral("'%1' is used by %2").arg(name, users.join(QStringLiteral(", ")))));
				return;
			}
			if(!self->checkpoints.contains(name))
			{
				respond(McpServer::ErrorResult(QStringLiteral("No checkpoint '%1'").arg(name)));
				return;
			}
			self->checkpoints.remove(name);
			QString error;
			if(!self->Save(&error))
			{
				respond(McpServer::ErrorResult(error));
				return;
			}
			respond(McpServer::TextResult(QStringLiteral("Deleted checkpoint '%1'").arg(name)));
		},
	});
}
