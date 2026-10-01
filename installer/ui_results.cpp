// What the final page says after an install or uninstall: plain facts first, then
// anything that needs the user's attention.

#include "wizard.h"

namespace ce::setup {

void AddResultLine(Wizard& wizard, COLORREF color, std::wstring text) {
    wizard.resultLines.push_back({color, std::move(text)});
}

void BuildInstallResult(Wizard& wizard) {
    const InstallResult& result = wizard.installResult;
    wizard.resultLines.clear();
    wizard.workSucceeded = result.success;
    if (result.success) {
        wizard.resultTitle = wizard.existing.state.installed ? L"Capture Engine was updated" : L"Capture Engine is ready";
        wizard.resultColor = result.warnings.empty() ? ui::kSuccess : ui::kCaution;
        AddResultLine(wizard, ui::kText, std::wstring(L"Version ") + kVersion + L" is installed in " + wizard.directory + L".");
        if (result.configWritten)
            AddResultLine(wizard, ui::kTextSecondary, L"A default config.ini was created.");
        if (result.templateWritten)
            AddResultLine(wizard, ui::kTextSecondary,
                    L"Your config.ini was kept as it was. The defaults of this version are in config.ini.new in "
                    L"the same folder, so you can compare them and copy new options over.");
        if (result.startupConfigured)
            AddResultLine(wizard, ui::kTextSecondary,
                    L"Startup and the elevation service are set as you chose; change them any time from the tray menu.");
        if (result.pawnIoInstalled)
            AddResultLine(wizard, ui::kTextSecondary, L"The PawnIO driver was installed.");
        for (const std::wstring& warning : result.warnings)
            AddResultLine(wizard, ui::kCaution, warning);
    } else {
        wizard.resultTitle = L"Setup did not finish";
        wizard.resultColor = ui::kCritical;
        AddResultLine(wizard, ui::kText, result.error.empty() ? L"Setup could not complete." : result.error);
        AddResultLine(wizard, ui::kTextSecondary, L"Your previous installation, if any, was left as it was.");
        if (!LogFilePath().empty())
            AddResultLine(wizard, ui::kTextSecondary, L"Details: " + LogFilePath());
    }
}

void BuildUninstallResult(Wizard& wizard) {
    const UninstallResult& result = wizard.uninstallResult;
    wizard.resultLines.clear();
    wizard.workSucceeded = result.success;
    if (result.success) {
        wizard.resultTitle = L"Capture Engine was removed";
        wizard.resultColor = result.warnings.empty() ? ui::kSuccess : ui::kCaution;
        AddResultLine(wizard, ui::kText,
                wizard.options & kOptRemoveData
                    ? L"The program, its service, startup entry and shortcuts, your config.ini and the logs were removed."
                    : L"The program, its service, startup entry and shortcuts were removed.");
        AddResultLine(wizard, ui::kTextSecondary,
                wizard.options & kOptRemoveData
                    ? L"Recordings, screenshots and benchmark reports in the install folder were not touched."
                    : L"Your config.ini, logs, recordings and screenshots were kept in the install folder.");
        if (result.rebootRecommended)
            AddResultLine(wizard, ui::kTextSecondary, L"A few files were in use and are deleted at the next restart.");
        for (const std::wstring& warning : result.warnings)
            AddResultLine(wizard, ui::kCaution, warning);
    } else {
        wizard.resultTitle = L"Uninstall did not finish";
        wizard.resultColor = ui::kCritical;
        AddResultLine(wizard, ui::kText, result.error.empty() ? L"Capture Engine could not be removed." : result.error);
        if (!LogFilePath().empty())
            AddResultLine(wizard, ui::kTextSecondary, L"Details: " + LogFilePath());
    }
}

}  // namespace ce::setup
