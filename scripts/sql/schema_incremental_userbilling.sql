-- 增量：在已有 CCT_CN 库上增加 UserBilling（若已存在则跳过创建）
USE CCT_CN;
GO

IF OBJECT_ID(N'dbo.UserBilling', N'U') IS NULL
BEGIN
  CREATE TABLE dbo.UserBilling (
    UserId BIGINT NOT NULL CONSTRAINT PK_UserBilling PRIMARY KEY,
    Tier NVARCHAR(16) NOT NULL CONSTRAINT DF_UserBilling_Tier DEFAULT (N'free'),
    TokenQuota BIGINT NOT NULL CONSTRAINT DF_UserBilling_Quota DEFAULT (50000),
    TokensConsumed BIGINT NOT NULL CONSTRAINT DF_UserBilling_Consumed DEFAULT (0),
    PeriodYm INT NOT NULL CONSTRAINT DF_UserBilling_Period DEFAULT (0),
    UpdatedAt DATETIME2 NOT NULL CONSTRAINT DF_UserBilling_Upd DEFAULT (SYSUTCDATETIME()),
    CONSTRAINT FK_UserBilling_User FOREIGN KEY (UserId) REFERENCES dbo.Users (Id) ON DELETE CASCADE
  );
END
GO
