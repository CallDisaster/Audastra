qt_add_library(War3MapQtFeedback STATIC
    FeedbackClient.cpp FeedbackClient.h FeedbackDialog.cpp FeedbackDialog.h
    FeedbackPreferences.cpp FeedbackPreviewDialog.cpp FeedbackQueuePage.cpp FeedbackSettingsSection.cpp
    FeedbackTransport.cpp FeedbackTransport.h FeedbackWorker.cpp FeedbackWorker.h FeedbackUi.h
    ProgramIssueReporter.cpp ProgramIssueReporter.h ProgramIssueApplication.h)
target_include_directories(War3MapQtFeedback PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtFeedback PUBLIC War3MapEditor::Core Qt6::Core Qt6::Widgets Qt6::Network PRIVATE audastra-version)
target_compile_definitions(War3MapQtFeedback PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtFeedback PRIVATE /W4 /permissive- /utf-8)
add_library(War3MapEditor::QtFeedback ALIAS War3MapQtFeedback)
