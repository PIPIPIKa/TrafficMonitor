#pragma once
#include "afxcmn.h"
#include "Common.h"
#include "HistoryTrafficListDlg.h"
#include "HistoryTrafficCalendarDlg.h"
#include "BaseDialog.h"

// CHistoryTrafficDlg 对话框

class CHistoryTrafficDlg : public CBaseDialog
{
    DECLARE_DYNAMIC(CHistoryTrafficDlg)

public:
    CHistoryTrafficDlg(const deque<HistoryTraffic>& history_traffics, bool captureActive, CWnd* pParent = NULL);   // 标准构造函数
    virtual ~CHistoryTrafficDlg();

    // 对话框数据
#ifdef AFX_DESIGN_TIME
    enum { IDD = IDD_HISTORY_TRAFFIC_DIALOG };
#endif

private:
    const deque<HistoryTraffic>& m_source;
    deque<HistoryTraffic> m_filtered;
    bool m_captureActive;
    CComboBox m_filter;
    afx_msg void OnScopeChanged();
public:
    CHistoryTrafficListDlg m_tab1_dlg;
    CHistoryTrafficCalendarDlg m_tab2_dlg;

protected:


    CTabCtrl m_tab;
    int m_tab_selected{};

protected:
    virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV 支持
    void SetTabWndSize();
    virtual CString GetDialogName() const override;

    DECLARE_MESSAGE_MAP()
public:
    virtual BOOL OnInitDialog();
    virtual BOOL PreTranslateMessage(MSG* pMsg);
    afx_msg void OnTcnSelchangeTab1(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnSize(UINT nType, int cx, int cy);
};
