package com.avatarforge.app

import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.lifecycle.ViewModel
import com.avatarforge.sdk.EngineResult
import com.avatarforge.sdk.JobHandle
import com.avatarforge.sdk.JobStatus

internal enum class Screen { INPUT, PROGRESS, VIEWER }

/** Activity-session state retained by AndroidX across configuration recreation, but not process death. */
internal class GenerationSessionState {
    val screen: MutableState<Screen> = mutableStateOf(Screen.INPUT)
    val job: MutableState<JobHandle?> = mutableStateOf(null)
    val status: MutableState<JobStatus> = mutableStateOf(JobStatus())
    val result: MutableState<EngineResult?> = mutableStateOf(null)
}

internal class GenerationSessionViewModel : ViewModel() {
    val state = GenerationSessionState()
}
