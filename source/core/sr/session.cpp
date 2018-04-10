#include "stdafx.h"
#include <future>
#include "session.h"
#include "guid_utils.h"
#include "service_helpers.h"


namespace CARBON_IMPL_NAMESPACE() {


CSpxSession::CSpxSession() :
    m_recoAsyncWaiting(false),
    m_sessionId(PAL::CreateGuid())
{
    SPX_DBG_TRACE_FUNCTION();
}

CSpxSession::~CSpxSession()
{
    SPX_DBG_TRACE_FUNCTION();
}

const std::wstring& CSpxSession::GetSessionId() const
{
    return m_sessionId;
}

void CSpxSession::AddRecognizer(std::shared_ptr<ISpxRecognizer> recognizer)
{
     m_recognizers.push_back(recognizer);
}

void CSpxSession::RemoveRecognizer(ISpxRecognizer* recognizer)
{
     m_recognizers.remove_if([&](std::weak_ptr<ISpxRecognizer>& item) {
         std::shared_ptr<ISpxRecognizer> sharedPtr = item.lock();
         return sharedPtr.get() == recognizer;
     });
}

CSpxAsyncOp<std::shared_ptr<ISpxRecognitionResult>> CSpxSession::RecognizeAsync()
{
    SPX_DBG_TRACE_FUNCTION();

    std::packaged_task<std::shared_ptr<ISpxRecognitionResult>()> task([=](){

        SPX_DBG_TRACE_SCOPE("*** CSpxSession::RecognizeAsync kicked-off THREAD started ***", "*** CSpxSession::RecognizeAsync kicked-off THREAD stopped ***");

        // Keep track of the fact that we have a thread hanging out waiting to hear
        // what the final recognition result is, and then stop recognizing...
        m_recoAsyncWaiting = true;
        this->StartRecognizing(RecognitionKind::SingleShot);

        // Wait for the recognition result, and then stop recognizing
        auto result = this->WaitForRecognition();
        this->StopRecognizing(RecognitionKind::SingleShot);

        // Return our result back to the future/caller
        return result;
    });

    auto taskFuture = task.get_future();
    std::thread taskThread(std::move(task));
    taskThread.detach();

    return CSpxAsyncOp<std::shared_ptr<ISpxRecognitionResult>>(
        std::forward<std::future<std::shared_ptr<ISpxRecognitionResult>>>(taskFuture),
        AOS_Started);
}

CSpxAsyncOp<void> CSpxSession::StartContinuousRecognitionAsync()
{
    return StartRecognitionAsync(RecognitionKind::Continuous);
}

CSpxAsyncOp<void> CSpxSession::StopContinuousRecognitionAsync()
{
    return StopRecognitionAsync(RecognitionKind::Continuous);
}

CSpxAsyncOp<void> CSpxSession::StartKeywordRecognitionAsync(const wchar_t* keyword)
{
    return StartRecognitionAsync(RecognitionKind::Keyword, keyword);
}

CSpxAsyncOp<void> CSpxSession::StopKeywordRecognitionAsync()
{
    return StopRecognitionAsync(RecognitionKind::Keyword);
}

CSpxAsyncOp<void> CSpxSession::StartRecognitionAsync(RecognitionKind startKind, std::wstring keyword)
{
    SPX_DBG_TRACE_FUNCTION();

    std::packaged_task<void()> task([=](){
        SPX_DBG_TRACE_SCOPE("*** CSpxSession::StartRecognitionAsync kicked-off THREAD started ***", "*** CSpxSession::StartRecognitionAsync kicked-off THREAD stopped ***");
        this->StartRecognizing(startKind, keyword);
    });

    auto taskFuture = task.get_future();
    std::thread taskThread(std::move(task));
    taskThread.detach();

    return CSpxAsyncOp<void>(
        std::forward<std::future<void>>(taskFuture),
        AOS_Started);    
}

CSpxAsyncOp<void> CSpxSession::StopRecognitionAsync(RecognitionKind stopKind)
{
    SPX_DBG_TRACE_FUNCTION();

    std::packaged_task<void()> task([=](){
        SPX_DBG_TRACE_SCOPE("*** CSpxSession::StopRecognitionAsync kicked-off THREAD started ***", "*** CSpxSession::StopRecognitionAsync kicked-off THREAD stopped ***");
        this->StopRecognizing(stopKind);
    });

    auto taskFuture = task.get_future();
    std::thread taskThread(std::move(task));
    taskThread.detach();

    return CSpxAsyncOp<void>(
        std::forward<std::future<void>>(taskFuture),
        AOS_Started);    
}

void CSpxSession::StartRecognizing(RecognitionKind startKind, std::wstring keyword)
{
    UNUSED(startKind);
    UNUSED(keyword);
    SPX_DBG_TRACE_SCOPE("Sleeping for 500ms...", "Sleeping for 500ms... Done!");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

void CSpxSession::StopRecognizing(RecognitionKind stopKind)
{
    UNUSED(stopKind);
    SPX_DBG_TRACE_SCOPE("Sleeping for 1000ms...", "Sleeping for 1000ms... Done!");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

std::shared_ptr<ISpxRecognitionResult> CSpxSession::WaitForRecognition()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait_for(lock, std::chrono::seconds(m_recoAsyncTimeout), [&] { return !m_recoAsyncWaiting; });

    if (m_recoAsyncResult == nullptr) // If we don't have a result, make a 'NoMatch' result
    {
        lock.unlock();
        EnsureFireResultEvent();
    }

    SPX_DBG_ASSERT(m_recoAsyncResult != nullptr);

    return std::move(m_recoAsyncResult);
}

void CSpxSession::WaitForRecognition_Complete(std::shared_ptr<ISpxRecognitionResult> result)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_recoAsyncWaiting)
    {
        m_recoAsyncWaiting = false;
        m_recoAsyncResult = result;

        m_cv.notify_all();
    }

    lock.unlock();
    FireResultEvent(GetSessionId(), result);
}

void CSpxSession::FireSessionStartedEvent()
{
    SPX_DBG_TRACE_FUNCTION();

    // Make a copy of the recognizers (under lock), to use to send events; 
    // otherwise the underlying list could be modified while we're sending events...

    std::unique_lock<std::mutex> lock(m_mutex);
    decltype(m_recognizers) weakRecognizers(m_recognizers.begin(), m_recognizers.end());
    lock.unlock();

    for (auto weakRecognizer : weakRecognizers)
    {
        auto recognizer = weakRecognizer.lock();
        auto ptr = std::dynamic_pointer_cast<ISpxRecognizerEvents>(recognizer);
        if (recognizer)
        {
            ptr->FireSessionStarted(m_sessionId);
        }
    }
}

void CSpxSession::FireSessionStoppedEvent()
{
    SPX_DBG_TRACE_FUNCTION();
    EnsureFireResultEvent();

    // Make a copy of the recognizers (under lock), to use to send events; 
    // otherwise the underlying list could be modified while we're sending events...

    std::unique_lock<std::mutex> lock(m_mutex);
    decltype(m_recognizers) weakRecognizers(m_recognizers.begin(), m_recognizers.end());
    lock.unlock();

    for (auto weakRecognizer : weakRecognizers)
    {
        auto recognizer = weakRecognizer.lock();
        auto ptr = std::dynamic_pointer_cast<ISpxRecognizerEvents>(recognizer);
        if (recognizer)
        {
            ptr->FireSessionStopped(m_sessionId);
        }
    }
}

void CSpxSession::FireResultEvent(const std::wstring& sessionId, std::shared_ptr<ISpxRecognitionResult> result)
{
    // Make a copy of the recognizers (under lock), to use to send events; 
    // otherwise the underlying list could be modified while we're sending events...

    std::unique_lock<std::mutex> lock(m_mutex);
    decltype(m_recognizers) weakRecognizers(m_recognizers.begin(), m_recognizers.end());
    lock.unlock();

    // BUG: why a result from a particualr recognizer needs to be fired on all recognizers in the session??
    // Why the adapter info is ignored in CSpxAudioStreamSession::FinalRecoResult??
    for (auto weakRecognizer : weakRecognizers)
    {
        auto recognizer = weakRecognizer.lock();
        auto ptr = std::dynamic_pointer_cast<ISpxRecognizerEvents>(recognizer);
        if (recognizer)
        {
            ptr->FireResultEvent(sessionId, result);
        }
    }
}

void CSpxSession::EnsureFireResultEvent()
{
    // Since we're not holding a lock throughout this "ensure" method, a race is still possible.
    // That said, the race is benign, in the worst case we just created a throw away no-match result.
    if (m_recoAsyncWaiting)
    {
        auto factory = SpxQueryService<ISpxRecoResultFactory>(this);
        auto noMatchResult = factory->CreateNoMatchResult(ResultType::Speech);
        WaitForRecognition_Complete(noMatchResult);
    }
}


} // CARBON_IMPL_NAMESPACE
