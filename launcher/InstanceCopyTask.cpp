#include "InstanceCopyTask.h"
#include "settings/INISettingsObject.h"
#include "FileSystem.h"
#include "NullInstance.h"
#include "pathmatcher/RegexpMatcher.h"
#include <QtConcurrentRun>

InstanceCopyTask::InstanceCopyTask(InstancePtr origInstance, const InstanceCopyPrefs& prefs)
{
    m_origInstance = origInstance;
    m_keepPlaytime = prefs.isKeepPlaytimeEnabled();

    QString filters = prefs.getSelectedFiltersAsRegex();

    m_useLinks = prefs.isLinkFilesEnabled();
    m_useHardLinks = prefs.isUseHardLinksEnabled();
    m_copySaves = prefs.isDontLinkSavesEnabled() && prefs.isCopySavesEnabled();

    if (!filters.isEmpty())
    {
        // Set regex filter:
        // FIXME: get this from the original instance type...
        auto matcherReal = new RegexpMatcher(filters);
        matcherReal->caseSensitive(false);
        m_matcher.reset(matcherReal);
    }
}

void InstanceCopyTask::executeTask()
{
    setStatus(tr("Copying instance %1").arg(m_origInstance->name()));

    auto copySaves = [&](){
        FS::copy savesCopy(FS::PathCombine(m_origInstance->instanceRoot(), "saves") , FS::PathCombine(m_stagingPath, "saves"));
        savesCopy.followSymlinks(false);

        return savesCopy();
    };

    m_copyFuture = QtConcurrent::run(QThreadPool::globalInstance(), [this, copySaves]{
        if (m_useLinks) {
            FS::create_link folderLink(m_origInstance->instanceRoot(), m_stagingPath);
            folderLink.linkRecursively(true).useHardLinks(m_useHardLinks).matcher(m_matcher.get());

            bool there_were_errors = false;

            if(!folderLink()){
#if defined Q_OS_WIN32
                if (!m_useHardLinks) {
                    qDebug() << "EXPECTED: Link failure, Windows requires permissions for symlinks";

                    qDebug() << "atempting to run with privelage";

                    QEventLoop loop;
                    bool got_priv_results = false;

                    connect(&folderLink, &FS::create_link::finishedPrivlaged, this, [&](bool gotResults){
                        if (!gotResults) {
                            qDebug() << "Privlaged run exited without results!";
                        }
                        got_priv_results = gotResults;
                        loop.quit();
                    });
                    folderLink.runPrivlaged();

                    loop.exec(); // wait for the finished signal

                    for (auto result : folderLink.getResults()) {
                        if (result.err_value != 0) {
                            there_were_errors = true;
                        }
                    }

                    if (m_copySaves) {
                        there_were_errors |= !copySaves();
                    }
                    
                    return got_priv_results && !there_were_errors;
                } else {
                    qDebug() << "Link Failed!" << folderLink.getOSError().value() << folderLink.getOSError().message().c_str();
                }      
#else
                qDebug() << "Link Failed!" << folderLink.getOSError().value() << folderLink.getOSError().message().c_str();
#endif          return false;
            }
            
            if (m_copySaves) {
                there_were_errors |= !copySaves();
            }

            return !there_were_errors;
        } else {
            FS::copy folderCopy(m_origInstance->instanceRoot(), m_stagingPath);
            folderCopy.followSymlinks(false).matcher(m_matcher.get());

            return folderCopy();
        }
    });
    connect(&m_copyFutureWatcher, &QFutureWatcher<bool>::finished, this, &InstanceCopyTask::copyFinished);
    connect(&m_copyFutureWatcher, &QFutureWatcher<bool>::canceled, this, &InstanceCopyTask::copyAborted);
    m_copyFutureWatcher.setFuture(m_copyFuture);
}

void InstanceCopyTask::copyFinished()
{
    auto successful = m_copyFuture.result();
    if(!successful)
    {
        emitFailed(tr("Instance folder copy failed."));
        return;
    }
    // FIXME: shouldn't this be able to report errors?
    auto instanceSettings = std::make_shared<INISettingsObject>(FS::PathCombine(m_stagingPath, "instance.cfg"));

    InstancePtr inst(new NullInstance(m_globalSettings, instanceSettings, m_stagingPath));
    inst->setName(name());
    inst->setIconKey(m_instIcon);
    if(!m_keepPlaytime) {
        inst->resetTimePlayed();
    }
    emitSucceeded();
}

void InstanceCopyTask::copyAborted()
{
    emitFailed(tr("Instance folder copy has been aborted."));
    return;
}
